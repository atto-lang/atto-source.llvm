#pragma once

// Experimental, opt-in source AST -> common-source ores-hir v1 export.
// NOT LLVM IR, not production codegen, and not a certified int ABI.
// Reject all native source features outside the tiny semantic intersection.
#include "oreslang/frontend/Parser.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace oreslang {

inline std::runtime_error unsupported_hir_v1(const std::string& reason) {
    return std::runtime_error("experimental HIR v1 rejects: " + reason);
}

inline bool hir_v1_identifier(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    auto first = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    auto rest = [&](char c) { return first(c) || (c >= '0' && c <= '9'); };
    if (!first(s.front())) return false;
    return std::all_of(s.begin() + 1, s.end(), rest);
}

inline std::string hir_v1_expr_json(const Expression& expr,
                                    std::size_t depth, std::size_t& nodes) {
    if (depth > 48 || ++nodes > 2048)
        throw unsupported_hir_v1("expression depth or size limit");
    using Kind = Expression::Kind;
    switch (expr.kind) {
        case Kind::Integer:
            return "{\"kind\":\"int\",\"value\":\"" + std::to_string(expr.integer) + "\"}";
        case Kind::Variable:
            if (!hir_v1_identifier(expr.name))
                throw unsupported_hir_v1("invalid variable identifier");
            return "{\"kind\":\"var\",\"name\":\"" + expr.name + "\"}";
        case Kind::Add: case Kind::Subtract: case Kind::Multiply: {
            if (!expr.left || !expr.right)
                throw unsupported_hir_v1("binary expression missing operand");
            const char* op = expr.kind == Kind::Add ? "add" :
                             expr.kind == Kind::Subtract ? "sub" : "mul";
            return std::string("{\"kind\":\"bin\",\"op\":\"") + op
                + "\",\"lhs\":" + hir_v1_expr_json(*expr.left, depth + 1, nodes)
                + ",\"rhs\":" + hir_v1_expr_json(*expr.right, depth + 1, nodes) + "}";
        }
        case Kind::Call: {
            if (!hir_v1_identifier(expr.name) || expr.arguments.size() > 32)
                throw unsupported_hir_v1("call identity/arity");
            // The experimental contract does not support named argument reordering.
            for (const auto& name : expr.argument_names)
                if (!name.empty()) throw unsupported_hir_v1("named call arguments");
            std::string json = "{\"kind\":\"call\",\"function\":\"" + expr.name + "\",\"args\":[";
            for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                if (!expr.arguments[i]) throw unsupported_hir_v1("null call argument");
                if (i) json += ',';
                json += hir_v1_expr_json(*expr.arguments[i], depth + 1, nodes);
            }
            return json + "]}";
        }
        default:
            throw unsupported_hir_v1("expression opcode (negation, ownership, effects, FFI, arrays, etc.)");
    }
}

inline std::string emit_experimental_hir_v1(const Program& program) {
    if (!program.module.empty() || !program.classes.empty() ||
        !program.actors.empty() || !program.cooperative_loops.empty() ||
        program.functions.empty() || program.functions.size() > 32) {
        throw unsupported_hir_v1("module, class, actor, scalar loop or function count");
    }
    std::vector<const Function*> functions;
    functions.reserve(program.functions.size());
    for (const auto& fn : program.functions) functions.push_back(&fn);
    std::sort(functions.begin(), functions.end(),
              [](const Function* a, const Function* b) { return a->name < b->name; });

    std::unordered_set<std::string> declared;
    std::string out = "{\"ir_family\":\"ores-hir\",\"schema_version\":\"1.0.0-experimental\","
                      "\"module_id\":\"source\",\"required_features\":"
                      "[\"bool.v1\",\"checked-i64.v1\",\"control-flow.v1\",\"functions.v1\"],"
                      "\"functions\":[";
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const Function& fn = *functions[i];
        if (!hir_v1_identifier(fn.name) || !declared.insert(fn.name).second)
            throw unsupported_hir_v1("duplicate/invalid function identity");
        if (fn.returns_void || !fn.result || fn.scalar_while || !fn.locals.empty() ||
            !fn.statements.empty() || !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
            !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
            !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
            fn.caught_throw_value || !fn.caught_throw_defers.empty() ||
            fn.has_declared_effects || fn.parameters.size() > 32) {
            throw unsupported_hir_v1("function body/effects/ownership outside subset");
        }
        if (i) out += ',';
        out += "{\"name\":\"" + fn.name + "\",\"params\":[";
        std::unordered_set<std::string> params;
        for (std::size_t p = 0; p < fn.parameters.size(); ++p) {
            const Parameter& arg = fn.parameters[p];
            if (!arg.class_name.empty() || arg.mode != Parameter::Mode::Value ||
                !hir_v1_identifier(arg.name) || !params.insert(arg.name).second) {
                throw unsupported_hir_v1("parameter name/type/borrow mode");
            }
            if (p) out += ',';
            out += "{\"name\":\"" + arg.name + "\",\"type\":\"i64\"}";
        }
        // The shared HIR verifier applies this budget PER function,
        // not across all functions in the module.
        std::size_t nodes = 0;
        out += "],\"result_type\":\"i64\",\"body\":"
            + hir_v1_expr_json(*fn.result, 0, nodes) + "}";
    }
    out += "]}";
    // HIR v1 requires a closed, acyclic, arity-correct call graph.
    // Native compiler admission is not automatically the same as HIR v1
    // admission; reject instead of exporting invalid HIR.
    std::unordered_map<std::string, const Function*> definitions;
    for (const auto& fn : program.functions) definitions.emplace(fn.name, &fn);
    std::unordered_map<std::string, std::unordered_set<std::string>> edges;
    std::function<void(const Expression&, std::unordered_set<std::string>&)> calls;
    calls = [&](const Expression& expr, std::unordered_set<std::string>& callees) {
        switch (expr.kind) {
            case Expression::Kind::Call: {
                const auto found = definitions.find(expr.name);
                if (found == definitions.end() ||
                    found->second->parameters.size() != expr.arguments.size()) {
                    throw unsupported_hir_v1("unknown callee or wrong call arity");
                }
                callees.insert(expr.name);
                for (const auto& arg : expr.arguments) calls(*arg, callees);
                return;
            }
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
                calls(*expr.left, callees);
                calls(*expr.right, callees);
                return;
            default: return;
        }
    };
    for (const auto& fn : program.functions) {
        calls(*fn.result, edges[fn.name]);
    }
    std::unordered_set<std::string> visiting, done;
    std::function<void(const std::string&)> check_cycle;
    check_cycle = [&](const std::string& name) {
        if (done.count(name)) return;
        if (!visiting.insert(name).second)
            throw unsupported_hir_v1("recursive function call graph");
        for (const auto& callee : edges.at(name)) check_cycle(callee);
        visiting.erase(name);
        done.insert(name);
    };
    for (const auto& fn : program.functions) check_cycle(fn.name);
    if (out.size() > 1'048'576) throw unsupported_hir_v1("IR exceeds 1 MiB");
    return out + "\n";
}

} // namespace oreslang
