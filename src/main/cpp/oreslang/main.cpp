#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/backend/TargetAdmission.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SemanticDependencies.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/runtime/TargetAbi.hpp"
#include "oreslang/runtime/CoreLog.hpp"
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <map>
#include <new>
#include <set>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iterator>
#include <stdexcept>
#include <string>
#include <array>
#include <string_view>

// Reject malformed Unicode (overlong encodings, surrogates, truncated input and
// code points above U+10FFFF) and binary NUL before native parser admission.
static void require_valid_source_utf8(std::string_view source) {
    for (std::size_t i = 0; i < source.size();) {
        const auto lead = static_cast<unsigned char>(source[i]);
        if (lead == 0) throw std::runtime_error("native source contains NUL byte");
        if (lead <= 0x7f) { ++i; continue; }

        std::size_t continuation_count = 0;
        unsigned char min_first = 0x80;
        unsigned char max_first = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            if (lead == 0xe0) min_first = 0xa0;
            if (lead == 0xed) max_first = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            if (lead == 0xf0) min_first = 0x90;
            if (lead == 0xf4) max_first = 0x8f;
        } else {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        if (source.size() - i <= continuation_count) {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte = static_cast<unsigned char>(source[i + j]);
            if (byte < (j == 1 ? min_first : 0x80) ||
                byte > (j == 1 ? max_first : 0xbf)) {
                throw std::runtime_error("native source is not valid UTF-8");
            }
        }
        i += continuation_count + 1;
    }
}

static bool paths_alias(const char* source_path, const std::string& report_path) {
    std::error_code source_error;
    std::error_code report_error;
    const auto source = std::filesystem::weakly_canonical(
        std::filesystem::path(source_path), source_error);
    const auto report = std::filesystem::weakly_canonical(
        std::filesystem::path(report_path), report_error);
    if (source_error || report_error)
        throw std::runtime_error("cannot safely resolve rt-report/source paths");
    if (source == report) return true;
    std::error_code identity_error;
    const bool same_file = std::filesystem::equivalent(source, report, identity_error);
    if (!identity_error) return same_file;
    if (identity_error == std::errc::no_such_file_or_directory) return false;
    throw std::runtime_error("cannot safely compare rt-report/source identity");
}

static std::set<oreslang::CallableId> tree_shake_function_calls(
        const oreslang::Function& function,
        std::size_t remaining_budget) {
    oreslang::SemanticScanBudget budget;
    budget.max_dependency_edges = remaining_budget;
    return oreslang::scan_semantic_dependencies(function, budget).direct_calls;
}

static void require_function_only_tree_shake_surface(
        const oreslang::Program& program) {
    if (!program.actors.empty())
        throw std::runtime_error(
            "tree shaking currently rejects actor declarations until actor roots are modeled");
    if (!program.classes.empty())
        throw std::runtime_error(
            "tree shaking currently rejects class declarations until class initializer roots are modeled");
    if (!program.cooperative_loops.empty())
        throw std::runtime_error(
            "tree shaking currently rejects cooperative loop declarations until continuation ABI roots are modeled");
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return static_cast<bool>(fn.scalar_while);
            }))
        throw std::runtime_error(
            "tree shaking currently rejects scalar continuation functions until continuation ABI roots are modeled");
}

static void tree_shake_functions(oreslang::Program& program, const std::vector<std::string>& requested_roots) {
    std::map<oreslang::CallableId, std::size_t> definitions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        const oreslang::CallableId key{
            function.name, function.parameters.size()};
        if (!definitions.emplace(key, i).second)
            throw std::runtime_error(
                "tree shaking requires unique native function name/arity identities");
    }

    std::vector<oreslang::CallableId> pending;
    std::set<oreslang::CallableId> reachable;
    if (!requested_roots.empty()) {
        for (const auto& requested : requested_roots) {
            std::vector<oreslang::CallableId> matches;
            for (const auto& function : program.functions) {
                if (function.name == requested)
                    matches.push_back(
                        oreslang::CallableId{
                            function.name, function.parameters.size()});
            }
            if (matches.empty())
                throw std::runtime_error("unknown tree-shake root: " + requested);
            if (matches.size() != 1)
                throw std::runtime_error("ambiguous tree-shake root: " + requested);
            if (reachable.insert(matches.front()).second) pending.push_back(matches.front());
        }
    } else {
        for (const auto& function : program.functions) {
            if (function.exported || function.name == "main") {
                const oreslang::CallableId key{
                    function.name, function.parameters.size()};
                if (reachable.insert(key).second) pending.push_back(key);
            }
            if (function.exported) {
                // LLVM names overloaded families according to family
                // cardinality. Preserve every sibling of a public overload in
                // conservative mode so removing a private sibling cannot
                // silently rename the surviving exported ABI symbol.
                for (const auto& sibling : program.functions) {
                    if (sibling.name != function.name) continue;
                    const oreslang::CallableId sibling_key{
                        sibling.name, sibling.parameters.size()};
                    if (reachable.insert(sibling_key).second)
                        pending.push_back(sibling_key);
                }
            }
        }
    }
    if (pending.empty())
        throw std::runtime_error(
            "tree shaking requires an explicit root, main, or at least one pub function");

    std::size_t scanned_edges = 0;
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto definition = definitions.find(pending[cursor]);
        if (definition == definitions.end())
            throw std::runtime_error("tree shaking found an unresolved function root");
        constexpr std::size_t kTreeShakeMaxEdges = 30000;
        if (scanned_edges >= kTreeShakeMaxEdges)
            throw std::runtime_error(
                "tree shaking call graph exceeds compiler edge budget");
        const auto calls = tree_shake_function_calls(
                program.functions[definition->second],
                kTreeShakeMaxEdges - scanned_edges);
        scanned_edges += calls.size();
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error(
                    "tree shaking found an unresolved direct call: " +
                    oreslang::analysis_callable_key(call));
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const oreslang::Function& function) {
                return !reachable.count(
                    oreslang::CallableId{
                        function.name, function.parameters.size()});
            }),
        program.functions.end());
}



int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    bool native_hard_rt_profile = false;
    bool profile_seen = false;
    bool rt_check_only = false;
    bool rt_entry_seen = false;
    bool rt_report_seen = false;
    std::string rt_entry = "main/0";
    std::string rt_report_path;
    bool tree_shake = false;
    std::vector<std::string> tree_shake_roots;
    const char* path = nullptr;
    // Host-only severity policy: last CLI flag overrides the environment.
    const char* envLevel = std::getenv("ORES_CORE_LOG_LEVEL");
    std::string coreLogLevel = envLevel ? envLevel : "off";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg.rfind("--profile=", 0) == 0) {
            if (profile_seen) { std::cerr << "duplicate profile option\n"; return 2; }
            profile_seen = true;
            const std::string profile = arg.substr(std::string_view("--profile=").size());
            if (profile != "native-hard-rt") {
                std::cerr << "unsupported native compiler profile: " << profile << '\n';
                return 2;
            }
            native_hard_rt_profile = true;
        } else if (arg == "--rt-check") {
            if (rt_check_only) { std::cerr << "duplicate rt-check option\n"; return 2; }
            rt_check_only = true;
            native_hard_rt_profile = true;
        } else if (arg.rfind("--rt-entry=", 0) == 0) {
            if (rt_entry_seen) { std::cerr << "duplicate rt-entry option\n"; return 2; }
            rt_entry_seen = true;
            rt_entry = arg.substr(std::string_view("--rt-entry=").size());
            if (rt_entry.empty()) { std::cerr << "rt-entry cannot be empty\n"; return 2; }
        } else if (arg.rfind("--rt-report=", 0) == 0) {
            if (rt_report_seen) { std::cerr << "duplicate rt-report option\n"; return 2; }
            rt_report_seen = true;
            rt_report_path = arg.substr(std::string_view("--rt-report=").size());
            if (rt_report_path.empty()) { std::cerr << "rt-report path cannot be empty\n"; return 2; }
        } else if (arg.rfind("--core-log-level=", 0) == 0) {
            coreLogLevel = arg.substr(std::string_view("--core-log-level=").size());
        } else if (arg == "--no-core-log") {
            coreLogLevel = "off";
        } else if (arg == "--experimental-rt-scalar-loop") {
            if (experimental_rt_scalar_loop) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            experimental_rt_scalar_loop = true;
        } else if (arg == "--experimental-owned-int") {
            if (experimental_owned_int) { std::cerr << "duplicate owned-int option\n"; return 2; }
            experimental_owned_int = true;
        } else if (arg == "--tree-shake") {
            if (tree_shake) { std::cerr << "duplicate tree-shake option\n"; return 2; }
            tree_shake = true;
        } else if (arg.rfind("--tree-shake-root=", 0) == 0) {
            const std::string root = arg.substr(18);
            if (root.empty()) { std::cerr << "empty tree-shake root\n"; return 2; }
            tree_shake_roots.push_back(root);
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            const std::string target = arg.substr(9);
            if (target == "host") {
                // Historical default: host triple and ABI remain inferred downstream.
            } else if (target == "thumbv7m-none-eabi") {
                cortex_m_target = oreslang::TargetAbi::parse(target).experimental_cortexm_ir();
            } else if (target == "wasm32" || target == "wasm32-unknown-unknown") {
                wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
            } else {
                try {
                    (void)oreslang::TargetAbi::parse(target);
                    std::cerr << "target has no certified IR/ABI lowering: " << target << '\n';
                } catch (const std::invalid_argument&) {
                    std::cerr << "unknown target triple: " << target << '\n';
                }
                return 2;
            }
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (!tree_shake && !tree_shake_roots.empty()) {
        std::cerr << "tree-shake-root requires --tree-shake\n";
        return 2;
    }
    if (rt_entry_seen && !native_hard_rt_profile && !rt_report_seen) {
        std::cerr << "rt-entry requires --rt-check, --profile=native-hard-rt, or --rt-report\n";
        return 2;
    }
    oreslang::CoreLog::Level admittedLevel = oreslang::CoreLog::Level::Off;
    if (!oreslang::CoreLog::parse_level(coreLogLevel, admittedLevel)) {
        std::cerr << "invalid core log level (off|error|warn|info|debug|trace)\n";
        return 2;
    }
    // Default/off does not allocate or initialize 8192 recorder slots.
    // Optional diagnostics must not make ordinary compilation fail on OOM.
    std::unique_ptr<oreslang::CoreLog> coreLog;
    if (admittedLevel != oreslang::CoreLog::Level::Off)
        coreLog.reset(new (std::nothrow) oreslang::CoreLog(admittedLevel));
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--tree-shake] [--tree-shake-root=name ...] [--profile=native-hard-rt] [--rt-check] [--rt-entry=name/arity] [--rt-report=path] [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] [--core-log-level=off|error|warn|info|debug|trace] [--no-core-log] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        const std::size_t kMaxSourceBytes = cortex_m_target ? 128u * 1024u : 8u * 1024u * 1024u;
        const auto text = read_regular_source(path, kMaxSourceBytes, cortex_m_target
            ? "cortex-m source exceeds 128 KiB limit"
            : "native source exceeds 8 MiB admission limit");
        require_valid_source_utf8(text);
        // Target/capability directives are lexical comments in the native
        // subset. Reject rather than silently discard unverified portability
        // constraints before parsing or emitting LLVM IR.
        if (text.find("// ores:") != std::string::npos) {
            throw std::runtime_error("native target admission unsupported: ores environment headers require compiler verification");
        }
        if (experimental_rt_scalar_loop && (wasm32_target || experimental_owned_int))
            throw std::runtime_error("scalar-loop frame requires native host and no owned-int experiment");
        if (cortex_m_target && text.size() > 128u * 1024u)
            throw std::runtime_error("cortex-m source exceeds 128 KiB limit");
        if (native_hard_rt_profile && (wasm32_target || cortex_m_target))
            throw std::runtime_error("native-hard-rt profile currently requires the host target");
        if (native_hard_rt_profile && experimental_owned_int)
            throw std::runtime_error("native-hard-rt profile rejects experimental owned heap lowering");

        auto ast = oreslang::parse_program(text, experimental_owned_int,
                                           experimental_rt_scalar_loop);

        if (rt_report_seen && path != nullptr && paths_alias(path, rt_report_path))
            throw std::runtime_error("rt-report path must not overwrite source input");

        if (native_hard_rt_profile || rt_report_seen) {
            const auto report = oreslang::analyze_native_hard_rt(ast, rt_entry);
            const std::string report_json = report.to_json();
            if (rt_report_seen) {
                std::ofstream output(rt_report_path, std::ios::out | std::ios::trunc);
                if (!output) throw std::runtime_error("cannot open rt-report output");
                output.exceptions(std::ios::badbit | std::ios::failbit);
                output << report_json;
                output.flush();
            }
            if (rt_check_only) {
                std::cout << report_json;
                std::cout.flush();
                if (coreLog) coreLog->dump(stderr);
                return report.source_admitted ? 0 : 1;
            }
            if (native_hard_rt_profile && !report.source_admitted) {
                throw std::runtime_error(
                    "native-hard-rt admission failed [" + report.diagnostic +
                    "]: " + report.reason);
            }
        }

        if (cortex_m_target) {
            oreslang::validate_cortex_m_source_program(
                ast,
                experimental_owned_int,
                experimental_rt_scalar_loop);
        }
        // Pure target/lowering admission is additive. Full LLVM emission
        // remains the authoritative backstop until equivalence is proven.
        oreslang::validate_target_admission(
            ast, wasm32_target, cortex_m_target);

        if (tree_shake) {
            // Tree shaking is an optimization, not conditional compilation.
            // Validate/codegen the complete admitted source once before pruning
            // so unsupported target features cannot hide in unreachable code.
            (void)oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target);

            // The shipped pass is deliberately function-only. Richer Program
            // owners have hidden semantic/ABI roots that are not represented
            // by the direct-call graph yet. Fail closed rather than deleting
            // a helper that is reachable only from an actor handler, class
            // initializer, or host-driven continuation.
            require_function_only_tree_shake_surface(ast);
            tree_shake_functions(ast, tree_shake_roots);
        }
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target);
        std::cout.flush();
        if (coreLog) coreLog->dump(stderr);
        return 0;
    } catch (const std::exception& error) {
        // No guest payload, exception message or source path enters CoreLog.
        if (coreLog)
            coreLog->record(oreslang::CoreLog::Level::Error,
                            oreslang::CoreLog::EventId::CompileFailed, 1);
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        if (coreLog) coreLog->dump(stderr);
        return 1;
    }
}
