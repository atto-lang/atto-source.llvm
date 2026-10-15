#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ExperimentalHirV1.hpp"
#include "oreslang/frontend/CheckedI64Directive.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <algorithm>
#include <fstream>
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



int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_emit_hir_v1 = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--experimental-emit-hir-v1") {
            if (experimental_emit_hir_v1) {
                std::cerr << "duplicate experimental HIR export option\n"; return 2;
            }
            experimental_emit_hir_v1 = true;
        } else if (arg == "--experimental-rt-scalar-loop") {
            if (experimental_rt_scalar_loop) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            experimental_rt_scalar_loop = true;
        } else if (arg == "--experimental-owned-int") {
            if (experimental_owned_int) { std::cerr << "duplicate owned-int option\n"; return 2; }
            experimental_owned_int = true;
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
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] [--experimental-emit-hir-v1] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        std::ifstream source(path, std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + path);
        }
        // Bound input before lexing and IR construction; istreambuf_iterator
        // used to grow memory without limit for attacker-controlled files.
        // Enforce the stricter Cortex-M budget while streaming, before allocating.
        const std::size_t kMaxSourceBytes = cortex_m_target ? 128u * 1024u : 8u * 1024u * 1024u;
        std::array<char, 8192> buffer{};
        std::string text;
        while (source) {
            source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = source.gcount();
            if (n <= 0) break;
            if (static_cast<std::size_t>(n) > kMaxSourceBytes - text.size())
                throw std::runtime_error(cortex_m_target
                    ? "cortex-m source exceeds 128 KiB limit"
                    : "native source exceeds 8 MiB admission limit");
            text.append(buffer.data(), static_cast<std::size_t>(n));
        }
        // A short final read normally sets eofbit and failbit. Other I/O
        // errors must be rejected rather than admitting a truncated source.
        if (source.bad() || (source.fail() && !source.eof()))
            throw std::runtime_error("failed reading Oreslang source");
        require_valid_source_utf8(text);
        const bool experimental_checked_i64 =
            oreslang::checked_i64_source_opt_in(text);
        if (experimental_checked_i64 && (wasm32_target || cortex_m_target
                || experimental_owned_int || experimental_rt_scalar_loop)) {
            throw std::runtime_error("checked-i64-v1 is currently host scalar only");
        }
        if (experimental_emit_hir_v1 && text.size() > 1'048'576)
            throw std::runtime_error("experimental HIR v1 source exceeds 1 MiB");
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
        if (experimental_emit_hir_v1 && (target_seen || experimental_owned_int || experimental_rt_scalar_loop))
            throw std::runtime_error("experimental HIR exporter cannot combine with target/owned/loop flags");
        auto ast = oreslang::parse_program(text, experimental_owned_int,
                                           experimental_rt_scalar_loop);
        if (experimental_emit_hir_v1) {
            // Emit only after the REAL native parser has admitted source grammar.
            // The exporter rejects any unsupported semantics, without LLVM fallback.
            std::cout << oreslang::emit_experimental_hir_v1(ast, experimental_checked_i64);
            return 0;
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
                    case Kind::Integer: case Kind::Variable: return true;
                    case Kind::Negate:
                        return expr.left && scalar(*expr.left, depth + 1, calls);
                    case Kind::Add: case Kind::Subtract: case Kind::Multiply:
                        return expr.left && expr.right &&
                               scalar(*expr.left, depth + 1, calls) &&
                               scalar(*expr.right, depth + 1, calls);
                    case Kind::Call:
                        if (expr.arguments.size() > max_parameters) return false;
                        calls.push_back(expr.name);
                        for (const auto& arg : expr.arguments)
                            if (!arg || !scalar(*arg, depth + 1, calls)) return false;
                        return true;
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
            for (const auto& fn : ast.functions) {
                if ((!ast.actors.empty() && fn.name != "main") ||
                    (ast.actors.empty() && fn.name == "main") ||
                    fn.returns_void || !fn.result ||
                    !fn.locals.empty() || !fn.statements.empty() ||
                    !fn.early_returns.empty() ||
                    (ast.actors.empty() && (!fn.actor_spawns.empty() ||
                        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                        !fn.actor_actions.empty() || !fn.actor_field_writes.empty())) ||
                    fn.scalar_while || fn.caught_throw_value ||
                    !fn.caught_throw_defers.empty() || fn.has_declared_effects)
                    throw std::runtime_error("cortex-m target rejects main, effects or non-scalar functions");
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
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target, experimental_checked_i64);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
