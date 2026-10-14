#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <string_view>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>


// flags-2-env exports these variables using .cli-flags.toml. Read them without
// depending on an ambient TOML parser in the compiler (the upstream wrapper
// owns TOML/argv validation). Invalid inherited values always fail closed.
static bool read_env_flag(const char* key, bool& output) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return true;
    const std::string_view value(raw);
    if (value == "true" || value == "1" || value == "yes") { output = true; return true; }
    if (value == "false" || value == "0" || value == "no") { output = false; return true; }
    std::cerr << "invalid boolean environment value for " << key << '\n';
    return false;
}

int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    if (!read_env_flag("ORES_LLVM_EXPERIMENTAL_OWNED_INT", experimental_owned_int)
            || !read_env_flag("ORES_LLVM_EXPERIMENTAL_RT_SCALAR_LOOP", experimental_rt_scalar_loop)) {
        return 2;
    }
    const char* env_target = std::getenv("ORES_LLVM_TARGET");
    std::string target_value = env_target ? env_target : "host";
    const char* env_cpu = std::getenv("ORES_LLVM_CPU");
    if (env_cpu && *env_cpu) {
        std::cerr << "explicit CPU feature selection not implemented: " << env_cpu << '\n';
        return 2;
    }
    bool owned_seen = false;
    bool scalar_seen = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--experimental-rt-scalar-loop" || arg == "--no-experimental-rt-scalar-loop") {
            if (scalar_seen) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            scalar_seen = true;
            experimental_rt_scalar_loop = arg == "--experimental-rt-scalar-loop";
        } else if (arg == "--experimental-owned-int" || arg == "--no-experimental-owned-int") {
            if (owned_seen) { std::cerr << "duplicate owned-int option\n"; return 2; }
            owned_seen = true;
            experimental_owned_int = arg == "--experimental-owned-int";
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            target_value = arg.substr(9);
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (target_value == "host") {
        // Default target and ABI are inferred downstream.
    } else if (target_value == "thumbv7m-none-eabi") {
        cortex_m_target = oreslang::TargetAbi::parse(target_value).experimental_cortexm_ir();
    } else if (target_value == "wasm32" || target_value == "wasm32-unknown-unknown") {
        wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
    } else {
        try {
            (void)oreslang::TargetAbi::parse(target_value);
            std::cerr << "target has no certified IR/ABI lowering: " << target_value << '\n';
        } catch (const std::invalid_argument&) {
            std::cerr << "unknown target triple: " << target_value << '\n';
        }
        return 2;
    }
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        std::ifstream source(path, std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + path);
        }
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        if (source.bad()) throw std::runtime_error("failed reading Oreslang source");
        // Target/capability directives are lexical comments in the native
        // subset. Reject rather than silently discard unverified portability
        // constraints before parsing or emitting LLVM IR.
        if (text.find("// ores:") != std::string::npos) {
            throw std::runtime_error("native target admission unsupported: ores environment headers require compiler verification");
        }
        if (experimental_rt_scalar_loop && (wasm32_target || experimental_owned_int))
            throw std::runtime_error("scalar-loop frame requires native host and no owned-int experiment");
        auto ast = oreslang::parse_program(text, experimental_owned_int,
                                           experimental_rt_scalar_loop);
        if (cortex_m_target) {
            // Deliberately small freestanding ABI: i64 scalar functions only.
            // Never lower native actors, heap regions or unverified host calls
            // merely because they happen to compile into some LLVM IR.
            if (experimental_owned_int || experimental_rt_scalar_loop ||
                !ast.module.empty() || !ast.classes.empty() || !ast.actors.empty() ||
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
            for (const auto& fn : ast.functions) {
                if (fn.name == "main" || fn.returns_void || !fn.result ||
                    !fn.locals.empty() || !fn.statements.empty() ||
                    !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
                    !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                    !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
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
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
