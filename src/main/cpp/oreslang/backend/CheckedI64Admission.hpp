#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {

// This is a *codegen* gate, not just an exporter check: the scalar overflow
// path currently exits at the host boundary and cannot unwind guest owners,
// defer/finally, actor turns, or borrowed resource regions. Reject any source
// that could require those semantics before emitting LLVM IR.
inline void verify_checked_i64_host_profile(const Program& program) {
    const auto reject = [](const char* reason) {
        throw std::runtime_error(std::string("checked-i64-v1: ") + reason);
    };
    if (!program.module.empty() || !program.classes.empty() ||
        !program.actors.empty() || !program.cooperative_loops.empty() ||
        program.functions.empty() || program.functions.size() > 32)
        reject("only 1..32 host scalar functions without modules/owners/loops are supported");

    std::unordered_map<std::string, const Function*> functions;
    for (const auto& fn : program.functions) {
        if (fn.name.empty() || !functions.emplace(fn.name, &fn).second)
            reject("duplicate/empty function identity");
        // The standalone checked profile owns a single OS boundary, which
        // emits an authenticated libc diagnostic. Reject a guest symbol that
        // could collide with the host declaration at LLVM link time.
        if (fn.name == "puts")
            reject("reserved host diagnostic function identity");
        if (fn.returns_void || !fn.result || fn.scalar_while ||
            !fn.locals.empty() || !fn.statements.empty() ||
            !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
            !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
            !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
            fn.caught_throw_value || !fn.catch_binding.empty() ||
            !fn.caught_throw_defers.empty() ||
            fn.has_declared_effects || fn.parameters.size() > 32)
            reject("function ownership, effects or non-scalar body unsupported");
        if (fn.name == "main" && !fn.parameters.empty())
            reject("main requires zero parameters");
        std::unordered_set<std::string> names;
        for (const auto& param : fn.parameters)
            if (param.name.empty() || !names.insert(param.name).second ||
                !param.class_name.empty() || param.mode != Parameter::Mode::Value)
                reject("parameters must be distinct primitive i64 values");
    }

    // Until a stable exported checked-result ABI exists, every checked
    // source module must have an entrypoint which owns and resets its status.
    if (!functions.count("main"))
        reject("host checked source requires a main entrypoint");

    std::unordered_map<std::string, std::vector<std::string>> calls;
    for (const auto& fn : program.functions) {
        std::unordered_set<std::string> parameters;
        for (const auto& param : fn.parameters)
            parameters.insert(param.name);
        std::size_t visited = 0;
        std::function<void(const Expression&, std::size_t)> validate =
            [&](const Expression& expr, std::size_t depth) {
                if (depth > 48 || ++visited > 2048)
                    reject("expression depth/size budget exceeded");
                using Kind = Expression::Kind;
                switch (expr.kind) {
                    case Kind::Integer: return;
                    case Kind::Variable:
                        if (!parameters.count(expr.name))
                            reject("unknown scalar operand");
                        return;
                    case Kind::Negate:
                        if (!expr.left) reject("null unary operand");
                        validate(*expr.left, depth + 1);
                        return;
                    case Kind::Add: case Kind::Subtract: case Kind::Multiply:
                        if (!expr.left || !expr.right)
                            reject("null arithmetic operand");
                        validate(*expr.left, depth + 1);
                        validate(*expr.right, depth + 1);
                        return;
                    case Kind::Call: {
                        const auto callee = functions.find(expr.name);
                        // The real native parser stores one empty label per
                        // positional argument. Reject only named/reordered
                        // arguments or a malformed label-vector length.
                        bool invalid_labels = !expr.argument_names.empty() &&
                            expr.argument_names.size() != expr.arguments.size();
                        for (const auto& label : expr.argument_names)
                            invalid_labels = invalid_labels || !label.empty();
                        if (callee == functions.end() ||
                            callee->second->parameters.size() != expr.arguments.size() ||
                            invalid_labels)
                            reject("unresolved, wrong-arity or named scalar call");
                        calls[fn.name].push_back(expr.name);
                        for (const auto& arg : expr.arguments) {
                            if (!arg) reject("null call argument");
                            validate(*arg, depth + 1);
                        }
                        return;
                    }
                    default: reject("ownership, field, array or other expression unsupported");
                }
            };
        validate(*fn.result, 0);
    }

    // Scalar emission uses direct native calls. Reject both direct and mutual
    // recursion until resumable typed exception propagation is implemented.
    std::unordered_map<std::string, unsigned> state;
    std::function<void(const std::string&)> check_calls =
        [&](const std::string& name) {
            auto& mark = state[name];
            if (mark == 1) reject("recursive scalar calls unsupported");
            if (mark == 2) return;
            mark = 1;
            for (const auto& callee : calls[name]) check_calls(callee);
            mark = 2;
        };
    for (const auto& fn : program.functions) check_calls(fn.name);
}

} // namespace oreslang
