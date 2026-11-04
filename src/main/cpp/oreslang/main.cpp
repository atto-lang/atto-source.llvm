#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/LibraryProfile.hpp"
#include "oreslang/frontend/CoreLibrarySources.hpp"
#include "oreslang/runtime/TargetAbi.hpp"
#include "oreslang/runtime/CoreLog.hpp"
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>
#include <iterator>
#include <stdexcept>
#include <string>
#include <array>
#include <string_view>


// Compiler failures can include guest-controlled tokens and filenames.
// Never emit terminal control bytes, unbounded exception text or the exact
// source filesystem path in a native compiler diagnostic.
static void print_safe_diagnostic(std::ostream& stream, std::string_view message,
                                  std::size_t limit) {
    const auto count = std::min(message.size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char byte = static_cast<unsigned char>(message[i]);
        stream.put(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?');
    }
    if (message.size() > limit) stream << "...[truncated]";
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
    // libc++ may report a generic identity error when the destination does
    // not exist yet. Check that case explicitly without swallowing failures.
    std::error_code exists_error;
    const bool report_exists = std::filesystem::exists(report, exists_error);
    if (!exists_error && !report_exists) return false;
    if (identity_error == std::errc::no_such_file_or_directory) return false;
    throw std::runtime_error("cannot safely compare rt-report/source identity");
}

static std::vector<std::string> tree_shake_function_calls(
        const oreslang::Function& function,
        std::size_t remaining_budget) {
    const auto facts =
            oreslang::realtime_detail::direct_facts(function, remaining_budget);
    if (facts.call_budget_exceeded)
        throw std::runtime_error(
            "tree shaking call graph exceeds compiler edge budget");
    return facts.calls;
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
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return fn.async_source_i64 ||
                    !fn.source_tasks.empty() ||
                    !fn.future_pipelines.empty();
            }))
        throw std::runtime_error(
            "tree shaking currently rejects Task/Future pipeline metadata edges until they participate in the semantic reachability graph");
}

static void tree_shake_functions(oreslang::Program& program, const std::vector<std::string>& requested_roots) {
    std::unordered_map<std::string, std::size_t> definitions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        const std::string key =
            oreslang::realtime_detail::function_key(
                function.name, function.parameters.size());
        if (!definitions.emplace(key, i).second)
            throw std::runtime_error(
                "tree shaking requires unique native function name/arity identities");
    }

    std::vector<std::string> pending;
    std::unordered_set<std::string> reachable;
    if (!requested_roots.empty()) {
        for (const auto& requested : requested_roots) {
            std::vector<std::string> matches;
            for (const auto& function : program.functions) {
                if (function.name == requested)
                    matches.push_back(oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size()));
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
                const std::string key =
                    oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size());
                if (reachable.insert(key).second) pending.push_back(key);
            }
            if (function.exported) {
                // LLVM names overloaded families according to family
                // cardinality. Preserve every sibling of a public overload in
                // conservative mode so removing a private sibling cannot
                // silently rename the surviving exported ABI symbol.
                for (const auto& sibling : program.functions) {
                    if (sibling.name != function.name) continue;
                    const std::string sibling_key =
                        oreslang::realtime_detail::function_key(
                            sibling.name, sibling.parameters.size());
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
        if (scanned_edges >= oreslang::realtime_detail::kMaxRealtimeCallEdges)
            throw std::runtime_error(
                "tree shaking call graph exceeds compiler edge budget");
        const auto calls = tree_shake_function_calls(
                program.functions[definition->second],
                oreslang::realtime_detail::kMaxRealtimeCallEdges - scanned_edges);
        scanned_edges += calls.size();
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error(
                    "tree shaking found an unresolved direct call: " + call);
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const oreslang::Function& function) {
                return !reachable.count(oreslang::realtime_detail::function_key(
                    function.name, function.parameters.size()));
            }),
        program.functions.end());
}




// Compiler-owned, source-admitted effect hints, v0. This intentionally reuses
// the native AST and the existing complete direct-facts walker; it never
// invokes LLVM code generation, guest execution or the hard-RT admission mode.
// Only a closed, nonrecursive, statically resolved subset can be "known".
// Unknown/partial source surfaces return INCONCLUSIVE (2), never a false PASS.
static int print_native_analysis_v0(const oreslang::Program& ast) {
    using oreslang::FunctionEffects;
    using oreslang::FunctionEffect;
    using oreslang::realtime_detail::function_key;
    struct Row {
        FunctionEffects direct{};
        std::vector<std::string> calls;
        bool unknown = false;
    };
    // The shared direct-facts walker is broader than its complete
    // exceptional-effect proof: checked arithmetic, deferred cleanup and
    // mutable locals can fail in ways its RT effect bits do not describe.
    // Only literal/variable/direct-call scalar expressions are complete in v0.
    const auto scalar_proven = [](const auto& self, const oreslang::Expression* expr)
            -> bool {
        if (!expr) return true;
        switch (expr->kind) {
            case oreslang::Expression::Kind::Integer:
            case oreslang::Expression::Kind::Boolean:
            case oreslang::Expression::Kind::Variable:
                return true;
            case oreslang::Expression::Kind::Call:
                if (expr->type_arguments_present || !expr->type_arguments.empty())
                    return false;
                for (const auto& argument : expr->arguments)
                    if (!self(self, argument.get())) return false;
                return true;
            default:
                return false;
        }
    };
    const auto function_proven = [&](const oreslang::Function& fn) -> bool {
        if (fn.has_declared_effects || fn.scalar_while ||
            !fn.locals.empty() || !fn.early_returns.empty() ||
            !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
            !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
            !fn.actor_field_writes.empty() || fn.caught_throw_value ||
            !fn.caught_throw_defers.empty() ||
            !fn.return_class.empty() || !fn.return_sum.empty() ||
            !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
            fn.async_source_i64 || !fn.ready_await_actor.empty() ||
            !fn.done_await_actor.empty() || !fn.ready_future_name.empty() ||
            !fn.ready_future_actor.empty() ||
            fn.ready_await_form != oreslang::NativeAwaitForm::None)
            return false;
        for (const auto& param : fn.parameters)
            if (!param.class_name.empty() || !param.sum_type.empty() ||
                !param.generic_type.empty() ||
                param.scalar_type == oreslang::ScalarType::String ||
                param.mode != oreslang::Parameter::Mode::Value) return false;
        for (const auto& statement : fn.statements) {
            if (statement.kind != oreslang::Statement::Kind::Return ||
                !scalar_proven(scalar_proven, statement.expression.get()))
                return false;
        }
        return scalar_proven(scalar_proven, fn.result.get());
    };
    std::map<std::string, Row> rows;
    const bool unknown_source_surface =
        !ast.sum_types.empty() || !ast.type_only_declarations.empty() ||
        !ast.imports.empty() || !ast.actors.empty() || !ast.classes.empty() ||
        !ast.cooperative_loops.empty() || !ast.generators.empty() ||
        !ast.contracts.empty() || !ast.conforms.empty();
    if (ast.functions.size() > 4096)
        throw std::runtime_error("analysis v0 function budget exceeded");
    std::size_t edges = 0;
    for (const auto& fn : ast.functions) {
        const auto key = function_key(fn.name, fn.parameters.size());
        if (rows.count(key))
            throw std::runtime_error("analysis v0 ambiguous callable identity");
        const auto remaining = std::size_t{30000} - edges;
        const auto facts = oreslang::realtime_detail::direct_facts(fn, remaining, &ast);
        if (facts.call_budget_exceeded)
            throw std::runtime_error("analysis v0 call graph budget exceeded");
        edges += facts.calls.size();
        Row row;
        row.direct = facts.effects;
        row.calls = facts.calls;
        row.unknown = unknown_source_surface || !function_proven(fn) || fn.native_internal ||
            !facts.effects.valid() || !facts.bounded_calls ||
            !facts.bounded_cleanup || facts.unsupported_runtime ||
            (fn.has_declared_effects && !fn.declared_effects.valid());
        rows.emplace(key, std::move(row));
    }
    struct Result { FunctionEffects effects{}; bool unknown = false; };
    std::map<std::string, Result> resolved;
    std::set<std::string> active;
    std::function<Result(const std::string&)> visit =
        [&](const std::string& key) -> Result {
            if (const auto found = resolved.find(key); found != resolved.end())
                return found->second;
            if (!active.insert(key).second)
                return {{}, true}; // cycle: never infer pure from recursion
            const auto row = rows.find(key);
            if (row == rows.end()) {
                active.erase(key);
                return {{}, true}; // unresolved/external call
            }
            Result result{row->second.direct, row->second.unknown};
            for (const auto& callee : row->second.calls) {
                const auto child = visit(callee);
                result.effects = result.effects.united(child.effects);
                result.unknown |= child.unknown;
            }
            active.erase(key);
            if (!result.effects.valid()) result.unknown = true;
            resolved.emplace(key, result);
            return result;
        };
    const std::pair<std::string_view, FunctionEffect> effect_names[] = {
        {"state.read", FunctionEffect::StateRead},
        {"state.write", FunctionEffect::StateWrite},
        {"suspend", FunctionEffect::Suspend},
        {"io", FunctionEffect::Io},
        {"alloc", FunctionEffect::Alloc},
        {"block", FunctionEffect::Block},
        {"ffi", FunctionEffect::Ffi},
        {"throw", FunctionEffect::Throw},
        {"raise", FunctionEffect::Raise},
        {"panic", FunctionEffect::Panic}
    };
    bool any_unknown = false;
    for (const auto& [key, row] : rows) {
        (void)row;
        const auto result = visit(key);
        any_unknown |= result.unknown;
        std::cout << "ORES-ANALYZE-V0\t";
        print_safe_diagnostic(std::cout, key, 256);
        std::cout << "\tstatus=" << (result.unknown ? "inconclusive" : "known")
                  << "\tknown_effects=";
        bool first = true;
        for (const auto& entry : effect_names) {
            if (!result.effects.has(entry.second)) continue;
            if (!first) std::cout << ',';
            std::cout << entry.first;
            first = false;
        }
        if (first) std::cout << "none";
        std::cout << '\n';
    }
    if (rows.empty()) {
        std::cout << "ORES-ANALYZE-V0\tstatus=inconclusive\treason=no-callables\n";
        any_unknown = true;
    }
    if (any_unknown)
        std::cerr << "oreslang-llvmc: analysis v0 inconclusive; unsupported or unresolved effect closure\n";
    std::cout.flush();
    return any_unknown ? 2 : 0;
}


int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    bool native_hard_rt_profile = false;
    bool profile_seen = false;
    std::optional<oreslang::frontend::LibraryProfile> library_profile;
    bool library_profile_seen = false;
    bool allocator_provider_seen = false;
    bool bounded_allocator_provider = false;
    bool rt_check_only = false;
    bool analyze_only = false;
    bool rt_entry_seen = false;
    bool rt_report_seen = false;
    bool constraint_descriptors_only = false;
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
        if (arg.rfind("--allocator-provider=", 0) == 0) {
            if (allocator_provider_seen) {
                std::cerr << "duplicate allocator-provider option\n";
                return 2;
            }
            allocator_provider_seen = true;
            const std::string provider =
                arg.substr(std::string_view("--allocator-provider=").size());
            if (provider != "bounded-arena") {
                std::cerr << "unsupported allocator provider: " << provider
                          << " (expected bounded-arena)\n";
                return 2;
            }
            bounded_allocator_provider = true;
        } else if (arg.rfind("--library-profile=", 0) == 0) {
            if (library_profile_seen) {
                std::cerr << "duplicate library-profile option\n";
                return 2;
            }
            library_profile_seen = true;
            const std::string profile =
                arg.substr(std::string_view("--library-profile=").size());
            library_profile = oreslang::frontend::parse_library_profile(profile);
            if (!library_profile.has_value()) {
                std::cerr << "unsupported library profile: " << profile
                          << " (expected core or std)\n";
                return 2;
            }
        } else if (arg.rfind("--profile=", 0) == 0) {
            if (profile_seen) { std::cerr << "duplicate profile option\n"; return 2; }
            profile_seen = true;
            const std::string profile = arg.substr(std::string_view("--profile=").size());
            if (profile != "native-hard-rt") {
                std::cerr << "unsupported native compiler profile: " << profile << '\n';
                return 2;
            }
            native_hard_rt_profile = true;
        } else if (arg == "--analyze") {
            if (analyze_only) { std::cerr << "duplicate analyze option\n"; return 2; }
            analyze_only = true;
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
        } else if (arg == "--constraint-descriptors") {
            if (constraint_descriptors_only) {
                std::cerr << "duplicate constraint-descriptors option\n";
                return 2;
            }
            constraint_descriptors_only = true;
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
    // Public library architecture has exactly two layers. For now the native
    // backend only certifies the freestanding core profile on targets whose ABI
    // is explicitly no-OS, and the std profile on the ordinary hosted target.
    // This is intentionally fail-closed until effect/capability closure can
    // prove additional combinations.
    const bool freestanding_library_target = wasm32_target || cortex_m_target;
    if (!library_profile.has_value()) {
        library_profile =
            oreslang::frontend::default_library_profile(freestanding_library_target);
    }
    const std::string_view library_profile_error =
        oreslang::frontend::library_profile_admission_error(
            *library_profile, freestanding_library_target);
    if (!library_profile_error.empty()) {
        std::cerr << library_profile_error;
        if (*library_profile == oreslang::frontend::LibraryProfile::Core) {
            std::cerr << " (wasm32-unknown-unknown or thumbv7m-none-eabi)";
        }
        std::cerr << "\n";
        return 2;
    }
    if (bounded_allocator_provider && !freestanding_library_target) {
        std::cerr << "bounded-arena allocator provider is certified only for freestanding targets\n";
        return 2;
    }
    if (bounded_allocator_provider &&
            *library_profile != oreslang::frontend::LibraryProfile::Core) {
        std::cerr << "allocator provider requires library-profile=core\n";
        return 2;
    }
    const std::uint32_t admitted_provider_bits =
        bounded_allocator_provider
            ? oreslang::frontend::core_provider_bit(
                oreslang::frontend::CoreProviderRequirement::Allocator)
            : 0u;

    if (analyze_only &&
            (constraint_descriptors_only || rt_check_only || rt_entry_seen ||
             rt_report_seen || native_hard_rt_profile || experimental_owned_int ||
             experimental_rt_scalar_loop || tree_shake || !tree_shake_roots.empty() ||
             target_seen || bounded_allocator_provider || allocator_provider_seen)) {
        std::cerr << "--analyze is a host no-codegen source-admission mode; cannot combine with codegen, RT or target flags\n";
        return 2;
    }
    if (constraint_descriptors_only &&
            (tree_shake || !tree_shake_roots.empty() || native_hard_rt_profile ||
             rt_check_only || rt_entry_seen || rt_report_seen ||
             experimental_owned_int || experimental_rt_scalar_loop ||
             target_seen || allocator_provider_seen)) {
        std::cerr << "constraint-descriptors is a metadata-only mode and cannot be combined with execution/codegen options\n";
        return 2;
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
        std::cerr << "usage: oreslang-llvmc [--analyze] [--allocator-provider=bounded-arena] [--library-profile=core|std] [--constraint-descriptors] [--tree-shake] [--tree-shake-root=name ...] [--profile=native-hard-rt] [--rt-check] [--rt-entry=name/arity] [--rt-report=path] [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] [--core-log-level=off|error|warn|info|debug|trace] [--no-core-log] <source.atto|source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    std::string source_module;
    std::string source_namespace;
    try {
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        const std::size_t kMaxSourceBytes = cortex_m_target ? 128u * 1024u : 8u * 1024u * 1024u;
        const char* source_overflow = cortex_m_target
            ? "cortex-m source exceeds 128 KiB limit"
            : "native source exceeds 8 MiB admission limit";
        const SourceIdentity expected_source_identity =
            capture_source_identity(path, kMaxSourceBytes, source_overflow);
        SourceIdentity source_identity;
        const auto text = read_regular_source(
            path, kMaxSourceBytes, source_overflow,
            &source_identity, &expected_source_identity);
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

        auto ast = oreslang::load_native_program(
            path, text, source_identity, experimental_owned_int,
            experimental_rt_scalar_loop, !wasm32_target && !cortex_m_target,
            admitted_provider_bits);
        source_module = ast.module;
        source_namespace = ast.namespace_name;

        if (analyze_only) {
            // This is after the real parser, linker and type/ownership gates.
            // Never emit IR or execute guest code in the analysis path.
            return print_native_analysis_v0(ast);
        }

        if (constraint_descriptors_only) {
            for (const auto& descriptor : ast.type_constraint_descriptors) {
                std::cout << "descriptor\t"
                          << descriptor.schema_id << '\t'
                          << descriptor.qualified_type_name << '\t'
                          << descriptor.representation << '\t'
                          << (descriptor.validation_required ? "true" : "false") << '\t'
                          << descriptor.fingerprint << '\t'
                          << descriptor.canonical << '\n';
            }
            std::cout.flush();
            return 0;
        }

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
            // Deliberately small freestanding ABI: i64 scalar functions only.
            // Never lower native actors, heap regions or unverified host calls
            // merely because they happen to compile into some LLVM IR.
            if (experimental_owned_int || experimental_rt_scalar_loop ||
                !ast.module.empty() || !ast.classes.empty() ||
                !ast.cooperative_loops.empty() || ast.functions.empty())
                throw std::runtime_error("cortex-m target supports only freestanding scalar functions");
            constexpr std::size_t max_functions = 64;
            constexpr std::size_t max_parameters = 8;
            constexpr std::size_t max_expression_nodes = 4096;
            constexpr std::size_t max_expression_depth = 32;
            constexpr std::size_t max_call_depth = 16;
            if (ast.functions.size() > max_functions)
                throw std::runtime_error("cortex-m function count exceeds 64");
            std::size_t expression_nodes = 0;
            std::unordered_map<std::string, std::vector<std::string>> edges;
            std::function<bool(const oreslang::Expression&, std::size_t,
                               std::vector<std::string>&)> scalar =
                [&](const oreslang::Expression& expr, std::size_t depth,
                    std::vector<std::string>& calls) -> bool {
                if (depth > max_expression_depth || ++expression_nodes > max_expression_nodes)
                    throw std::runtime_error("cortex-m expression budget exceeded");

                using Kind = oreslang::Expression::Kind;
                switch (expr.kind) {
                    case Kind::Integer: case Kind::Boolean:
                    case Kind::Variable: return true;
                    case Kind::Negate: case Kind::LogicalNot:
                        return expr.left && scalar(*expr.left, depth + 1, calls);
                    case Kind::Add: case Kind::Subtract: case Kind::Multiply:
                    case Kind::Equal: case Kind::NotEqual:
                    case Kind::Less: case Kind::LessEqual:
                    case Kind::Greater: case Kind::GreaterEqual:
                    case Kind::LogicalAnd: case Kind::LogicalOr:
                    case Kind::LogicalXor:
                        return expr.left && expr.right &&
                               scalar(*expr.left, depth + 1, calls) &&
                               scalar(*expr.right, depth + 1, calls);
                    case Kind::Call:
                        if (expr.arguments.size() > max_parameters) return false;
                        calls.push_back(expr.name);
                        for (const auto& arg : expr.arguments)
                            if (!arg || !scalar(*arg, depth + 1, calls)) return false;
                        return true;
                    case Kind::ArrayCall:
                        if (!bounded_allocator_provider || !expr.left ||
                                expr.left->kind != Kind::Variable ||
                                expr.name != "get" ||
                                expr.arguments.size() != 1 ||
                                !expr.arguments.front())
                            return false;
                        return scalar(
                            *expr.arguments.front(), depth + 1, calls);
                    case Kind::Field:
                        return bounded_allocator_provider && expr.left &&
                            expr.left->kind == Kind::Variable &&
                            expr.name == "size";
                    default: return false;
                }
            };
            if (!ast.actors.empty()) {
                if (ast.actors.size() > 8 || ast.functions.size() != 1 ||
                    ast.functions.front().name != "main")
                    throw std::runtime_error("cortex-m actors require 1..8 actor types and only main");
                const auto& start = ast.functions.front();
                if (start.returns_void || !start.result ||
                    start.result->kind != oreslang::Expression::Kind::Integer ||
                    !start.locals.empty() || !start.statements.empty() ||
                    !start.early_returns.empty() || start.has_declared_effects ||
                    start.actor_spawns.empty() || start.actor_spawns.size() > 8 ||
                    start.actor_sends.size() > 32 ||
                    start.actor_stops.size() != start.actor_spawns.size() ||
                    !start.parameters.empty())
                    throw std::runtime_error("cortex-m actor bootstrap requires bounded source main");
                if (start.actor_actions.size() !=
                        start.actor_sends.size() + start.actor_stops.size())
                    throw std::runtime_error("cortex-m actor action plan inconsistent");
                for (const auto& actor : ast.actors) {
                    if (actor.fields.empty() || actor.fields.size() > 8 ||
                        actor.on_message.actor_field_writes.size() > 16 ||
                        !actor.on_message.result)
                        throw std::runtime_error("cortex-m actor field or handler budget exceeded");
                    for (const auto& field : actor.fields) {
                        if (!field.initializer ||
                            field.initializer->kind != oreslang::Expression::Kind::Integer)
                            throw std::runtime_error("cortex-m actors need literal i64 field initialization");
                    }
                    std::vector<std::string> calls;
                    if (!scalar(*actor.on_message.result, 0, calls) || !calls.empty())
                        throw std::runtime_error("cortex-m actor return must be scalar without guest calls");
                    for (const auto& write : actor.on_message.actor_field_writes) {
                        calls.clear();
                        if (!write.expression || !scalar(*write.expression, 0, calls) ||
                            !calls.empty())
                            throw std::runtime_error("cortex-m actor writes must be scalar without guest calls");
                    }
                }
                for (const auto& send : start.actor_sends) {
                    if (!send.payload ||
                        send.payload->kind != oreslang::Expression::Kind::Integer)
                        throw std::runtime_error("cortex-m actor send must be a literal i64");
                }
                // All bootstrap effects are statically ordered. Prove no
                // mailbox can overflow before lowering irreversible void sends.
                // The runtime's 16-entry cap is an ABI contract, not a hint.
                std::unordered_map<std::string, std::size_t> pending;
                std::unordered_set<std::string> stopped;
                for (const auto& spawn : start.actor_spawns) {
                    if (!pending.emplace(spawn.name, 0).second)
                        throw std::runtime_error("cortex-m duplicate actor ref");
                }
                for (const auto& action : start.actor_actions) {
                    std::string ref;
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (action.index >= start.actor_sends.size())
                            throw std::runtime_error("cortex-m invalid actor send index");
                        ref = start.actor_sends[action.index].reference;
                    } else {
                        if (action.index >= start.actor_stops.size())
                            throw std::runtime_error("cortex-m invalid actor stop index");
                        ref = start.actor_stops[action.index];
                    }
                    const auto it = pending.find(ref);
                    if (it == pending.end() || stopped.count(ref))
                        throw std::runtime_error("cortex-m invalid or stopped actor capability");
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (++it->second > 16)
                            throw std::runtime_error("cortex-m static mailbox capacity exceeded (16)");
                    } else {
                        stopped.insert(ref);
                    }
                }
                if (stopped.size() != pending.size())
                    throw std::runtime_error("cortex-m bootstrap must stop all actors");
                // The trusted C runtime owns static actors, mailboxes, and
                // orderly stop/drain; this does not admit guest awaits.
            }
            auto guarded_scalar_returns = [&](const oreslang::Function& fn) {
                if (fn.statements.size() > 8) return false;
                for (const auto& statement : fn.statements) {
                    if (statement.kind != oreslang::Statement::Kind::Conditional ||
                            !statement.condition ||
                            statement.condition_lhs || statement.condition_rhs ||
                            !statement.else_statements.empty() ||
                            statement.then_statements.size() != 1)
                        return false;
                    const auto& terminal = statement.then_statements.front();
                    if (terminal.kind != oreslang::Statement::Kind::Return ||
                            !terminal.expression ||
                            terminal.condition || terminal.condition_lhs ||
                            terminal.condition_rhs ||
                            !terminal.then_statements.empty() ||
                            !terminal.else_statements.empty())
                        return false;
                    std::vector<std::string> condition_calls;
                    if (!scalar(*statement.condition, 0, condition_calls) ||
                            !condition_calls.empty())
                        return false;
                    std::vector<std::string> return_calls;
                    if (!scalar(*terminal.expression, 0, return_calls))
                        return false;
                }
                return true;
            };
            auto bounded_allocator_function =
                [&](const oreslang::Function& fn) {
                    if (!bounded_allocator_provider ||
                            fn.locals.empty() || fn.locals.size() > 20 ||
                            !fn.statements.empty())
                        return false;
                    std::unordered_set<std::string> arrays;
                    std::size_t owners = 0;
                    std::size_t effects = 0;
                    for (const auto& local : fn.locals) {
                        if (local.int_array) {
                            if (++owners > 4 || !local.owner_mutable ||
                                    local.heap_owned || local.new_class ||
                                    !local.class_name.empty() ||
                                    local.initializer)
                                return false;
                            if (!arrays.insert(local.name).second)
                                return false;
                            continue;
                        }
                        if (local.name.rfind("$effect", 0) != 0 ||
                                !local.initializer ||
                                local.initializer->kind !=
                                    oreslang::Expression::Kind::ArrayCall ||
                                ++effects > 16)
                            return false;
                        const auto& call = *local.initializer;
                        if (!call.left ||
                                call.left->kind !=
                                    oreslang::Expression::Kind::Variable ||
                                !arrays.count(call.left->name))
                            return false;
                        const std::size_t expected =
                            call.name == "add" ? 1 :
                            (call.name == "set" ? 2 : 0);
                        if (expected == 0 ||
                                call.arguments.size() != expected)
                            return false;
                        for (const auto& argument : call.arguments) {
                            std::vector<std::string> calls;
                            if (!argument ||
                                    !scalar(*argument, 0, calls) ||
                                    !calls.empty())
                                return false;
                        }
                    }
                    return owners != 0;
                };
            for (const auto& fn : ast.functions) {
                const bool scalar_function =
                    fn.locals.empty() && guarded_scalar_returns(fn);
                const bool allocator_function =
                    bounded_allocator_function(fn);
                if ((!ast.actors.empty() && fn.name != "main") ||
                    (ast.actors.empty() && fn.name == "main") ||
                    fn.returns_void || !fn.result ||
                    (!scalar_function && !allocator_function) ||
                    !fn.early_returns.empty() ||
                    (ast.actors.empty() && (!fn.actor_spawns.empty() ||
                        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                        !fn.actor_actions.empty() || !fn.actor_field_writes.empty())) ||
                    fn.scalar_while || fn.caught_throw_value ||
                    !fn.caught_throw_defers.empty() || fn.has_declared_effects)
                    throw std::runtime_error(
                        "cortex-m target rejects main, effects or unbounded functions");
                if (fn.parameters.size() > max_parameters)
                    throw std::runtime_error("cortex-m parameter budget exceeded");
                for (const auto& param : fn.parameters)
                    if (!param.class_name.empty() ||
                        param.mode != oreslang::Parameter::Mode::Value)
                        throw std::runtime_error("cortex-m target requires value int parameters");
                std::vector<std::string> calls;
                if (!scalar(*fn.result, 0, calls))
                    throw std::runtime_error("cortex-m target requires bounded scalar expressions");
                if (!edges.emplace(fn.name, std::move(calls)).second)
                    throw std::runtime_error("cortex-m target rejects overloaded function names");
            }
            // Source-level cycles are forbidden on tiny stacks. The call-graph
            // path limit caps nesting even when individual ASTs are shallow.
            std::unordered_set<std::string> active;
            std::unordered_map<std::string, std::size_t> longest;
            std::function<std::size_t(const std::string&)> walk =
                [&](const std::string& name) -> std::size_t {
                const auto known = longest.find(name);
                if (known != longest.end()) return known->second;
                if (!active.insert(name).second)
                    throw std::runtime_error("cortex-m recursive call graph rejected");
                auto node = edges.find(name);
                if (node == edges.end())
                    throw std::runtime_error("cortex-m unresolved function call");
                std::size_t depth = 1;
                for (const auto& dependency : node->second) {
                    const auto child = walk(dependency);
                    if (child >= max_call_depth)
                        throw std::runtime_error("cortex-m call depth budget exceeded");
                    depth = std::max(depth, child + 1);
                }
                active.erase(name);
                longest.emplace(name, depth);
                return depth;
            };
            for (const auto& fn : ast.functions) (void)walk(fn.name);
        }
        if (tree_shake) {
            // Tree shaking is an optimization, not conditional compilation.
            // Validate/codegen the complete admitted source once before pruning
            // so unsupported target features cannot hide in unreachable code.
            (void)oreslang::emit_verified_llvm_ir(
                ast, wasm32_target, cortex_m_target,
                bounded_allocator_provider);

            // The shipped pass is deliberately function-only. Richer Program
            // owners have hidden semantic/ABI roots that are not represented
            // by the direct-call graph yet. Fail closed rather than deleting
            // a helper that is reachable only from an actor handler, class
            // initializer, or host-driven continuation.
            require_function_only_tree_shake_surface(ast);
            tree_shake_functions(ast, tree_shake_roots);
        }
        std::cout << oreslang::emit_verified_llvm_ir(
                ast, wasm32_target, cortex_m_target,
                bounded_allocator_provider);
        std::cout.flush();
        if (coreLog) coreLog->dump(stderr);
        return 0;
    } catch (const std::exception& error) {
        // No guest payload, exception message or source path enters CoreLog.
        if (coreLog)
            coreLog->record(oreslang::CoreLog::Level::Error,
                            oreslang::CoreLog::EventId::CompileFailed, 1);
        std::cerr << "oreslang-llvmc: ";
        std::string diagnostic = error.what();
        if (path != nullptr && *path != '\0') {
            const std::string source_path(path);
            // Full host paths are private CI metadata, not error context.
            // Replace exact input-path occurrences before rendering.
            std::size_t found = 0;
            while ((found = diagnostic.find(source_path, found)) != std::string::npos) {
                diagnostic.replace(found, source_path.size(), "<source>");
                found += sizeof("<source>") - 1;
            }
        }
        print_safe_diagnostic(std::cerr, diagnostic, 2048);
        if (path != nullptr) {
            std::string_view source_name(path);
            const auto slash = source_name.find_last_of("/\\");
            if (slash != std::string_view::npos) source_name.remove_prefix(slash + 1);
            std::cerr << " [file: ";
            print_safe_diagnostic(std::cerr, source_name, 128);
            std::cerr << "]";
        }
        if (!source_module.empty()) {
            std::cerr << " [module: ";
            print_safe_diagnostic(std::cerr, source_module, 128);
            std::cerr << "]";
        }
        if (!source_namespace.empty()) {
            std::cerr << " [namespace: ";
            print_safe_diagnostic(std::cerr, source_namespace, 128);
            std::cerr << "]";
        }
        std::cerr << '\n';
        if (coreLog) coreLog->dump(stderr);
        return 1;
    }
}
