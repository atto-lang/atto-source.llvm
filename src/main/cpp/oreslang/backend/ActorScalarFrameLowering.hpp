#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/SourceActorScalarVM.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang {

// Compiler-only M3 admission. No guest runtime names are resolved by dynamic
// string lookup: each closed, source-local fnc graph has fixed entry offsets.
struct ActorScalarCompiledProgram {
    std::vector<OresActorScalarInstruction> code;
    std::vector<std::uint64_t> entries;
    std::uint64_t root = 0;
};

namespace actor_scalar_m3 {
[[noreturn]] inline void deny(const char* reason) {
    throw std::runtime_error(
        std::string("native actor typed scalar frame admission: ") + reason);
}
inline bool contains_call(const Expression& e, unsigned depth = 0) {
    if (depth > 64) deny("expression depth exceeds closed graph limit");
    if (e.kind == Expression::Kind::Call) return true;
    auto child = [&](const std::unique_ptr<Expression>& v) {
        return v && contains_call(*v, depth + 1);
    };
    if (child(e.left) || child(e.right) || child(e.condition)
        || child(e.when_true) || child(e.when_false)
        || child(e.condition_lhs) || child(e.condition_rhs))
        return true;
    for (const auto& arg : e.arguments)
        if (child(arg)) return true;
    return false;
}

struct Builder {
    const Program& program;
    std::vector<const Function*> funcs;
    std::unordered_map<std::string, std::uint64_t> ids;
    ActorScalarCompiledProgram out;

    explicit Builder(const Program& p) : program(p) {}

    std::uint64_t enroll(const std::string& name) {
        const auto found = ids.find(name);
        if (found != ids.end()) return found->second;
        if (funcs.size() >= ORES_SCALAR_MAX_FUNCTIONS)
            deny("recursive function graph exceeds 16 declarations");
        const Function* candidate = nullptr;
        for (const auto& fn : program.functions)
            if (fn.name == name && fn.parameters.size() == 1) {
                if (candidate) deny("ambiguous unary function name/arity");
                candidate = &fn;
            }
        if (!candidate) deny("reachable scalar function is missing");
        const std::uint64_t next = funcs.size();
        ids.emplace(name, next);
        funcs.push_back(candidate);
        return next;
    }

    // Reaches every possible call edge without executing guest instructions.
    void scan(const Expression& e, unsigned depth = 0) {
        if (depth > 48) deny("expression AST too deep for typed frames");
        using K = Expression::Kind;
        switch (e.kind) {
            case K::Integer:
            case K::Variable:
                return;
            case K::Negate:
                if (!e.left) deny("malformed scalar negate");
                scan(*e.left, depth + 1);
                return;
            case K::LogicalNot:
                if (!e.left) deny("malformed boolean not");
                scan(*e.left, depth + 1);
                return;
            case K::Add:
            case K::Subtract:
            case K::Multiply:
            case K::Equal:
            case K::NotEqual:
            case K::Less:
            case K::LessEqual:
            case K::Greater:
            case K::GreaterEqual:
                if (!e.left || !e.right) deny("malformed scalar binary");
                scan(*e.left, depth + 1);
                scan(*e.right, depth + 1);
                return;
            case K::Call:
                if (e.arguments.size() != 1 || e.type_arguments_present
                    || !e.type_arguments.empty()
                    || (!e.argument_names.empty()
                       && (e.argument_names.size() != 1
                           || !e.argument_names[0].empty())))
                    deny("only positional unary same-source calls are admitted");
                if (!e.arguments[0]) deny("missing scalar call argument");
                scan(*e.arguments[0], depth + 1);
                enroll(e.name);
                return;
            default:
                deny("unsupported type, ownership, expression or effect");
        }
    }

    void validate_function(const Function& fn) {
        if (fn.name.empty() || fn.method_owner.size() || fn.native_internal
            || fn.async_source_i64 || fn.has_declared_effects
            || !fn.generic_parameters.empty() || fn.scalar_while
            || fn.returns_void || fn.return_type != ScalarType::I64
            || !fn.return_class.empty() || !fn.return_sum.empty()
            || !fn.pending_return_type_alias.empty()
            || fn.parameters.size() != 1
            || fn.parameters[0].mode != Parameter::Mode::Value
            || fn.parameters[0].scalar_type != ScalarType::I64
            || !fn.parameters[0].class_name.empty()
            || !fn.parameters[0].sum_type.empty()
            || !fn.parameters[0].generic_type.empty()
            || fn.statements.size() > 8
            || fn.locals.size() > ORES_SCALAR_MAX_LOCAL_SLOTS
            || !fn.actor_spawns.empty() || !fn.actor_sends.empty()
            || !fn.actor_stops.empty() || !fn.actor_actions.empty()
            || !fn.actor_field_writes.empty() || !fn.source_tasks.empty()
            || !fn.future_pipelines.empty() || !fn.ready_await_actor.empty()
            || !fn.done_await_actor.empty() || fn.early_returns.size() > 8
            || fn.caught_throw_value || !fn.caught_throw_defers.empty()
            || !fn.result)
            deny("function graph is not a closed pure unary integer fnc subset");
        for (const auto& local : fn.locals) {
            if (!local.initializer || local.owner_mutable || local.assignable
                || local.heap_owned || local.int_array || local.record_view
                || local.new_class || local.move_class
                || !local.class_name.empty() || !local.sum_type.empty()
                || !local.pending_type_alias.empty()
                || local.scalar_type != ScalarType::I64
                || !local.constructor_arguments.empty()
                || !local.task_await_name.empty()
                || !local.future_await_name.empty())
                deny("scalar frame local requires immutable i64 value");
            scan(*local.initializer);
        }
        for (const auto& guard : fn.early_returns) {
            if (!guard.condition || !guard.result)
                deny("legacy equality-only return guards unsupported");
            scan(*guard.condition);
            scan(*guard.result);
        }
        for (const auto& statement : fn.statements) {
            if (statement.kind != Statement::Kind::Conditional
                || !statement.condition || !statement.else_statements.empty()
                || statement.then_statements.size() != 1
                || statement.then_statements[0].kind != Statement::Kind::Return
                || !statement.then_statements[0].expression)
                deny("only pure conditional early returns are admitted");
            scan(*statement.condition);
            scan(*statement.then_statements[0].expression);
        }
        scan(*fn.result);
    }

    void emit(std::uint64_t opcode, std::int64_t immediate = 0) {
        if (out.code.size() >= ORES_SCALAR_MAX_INSTRUCTIONS)
            deny("function program exceeds 4096 instructions");
        out.code.push_back({opcode, immediate});
    }

    void expression(const Expression& e, const Function& fn,
                    const std::unordered_map<std::string, std::size_t>& locals,
                    unsigned depth = 0) {
        if (depth > 48) deny("emitted expression too deep");
        using K = Expression::Kind;
        switch (e.kind) {
            case K::Integer:
                emit(ORES_SCALAR_PUSH_I64, e.integer);
                return;
            case K::Variable:
                if (e.name == fn.parameters[0].name) {
                    emit(ORES_SCALAR_LOAD_ARG);
                    return;
                }
                if (const auto it = locals.find(e.name); it != locals.end()) {
                    emit(ORES_SCALAR_LOAD_LOCAL, (std::int64_t)it->second);
                    return;
                }
                deny("undefined/forward scalar local variable");
            case K::Negate:
                emit(ORES_SCALAR_PUSH_I64, 0);
                expression(*e.left, fn, locals, depth + 1);
                emit(ORES_SCALAR_SUB);
                return;
            case K::LogicalNot:
                expression(*e.left, fn, locals, depth + 1);
                emit(ORES_SCALAR_NOT);
                return;
            case K::Call:
                expression(*e.arguments[0], fn, locals, depth + 1);
                emit(ORES_SCALAR_CALL, (std::int64_t)ids.at(e.name));
                return;
            default: {
                expression(*e.left, fn, locals, depth + 1);
                expression(*e.right, fn, locals, depth + 1);
                std::uint64_t opcode = 0;
                switch (e.kind) {
                    case K::Add: opcode = ORES_SCALAR_ADD; break;
                    case K::Subtract: opcode = ORES_SCALAR_SUB; break;
                    case K::Multiply: opcode = ORES_SCALAR_MUL; break;
                    case K::Equal: opcode = ORES_SCALAR_EQ; break;
                    case K::NotEqual: opcode = ORES_SCALAR_NE; break;
                    case K::Less: opcode = ORES_SCALAR_LT; break;
                    case K::LessEqual: opcode = ORES_SCALAR_LE; break;
                    case K::Greater: opcode = ORES_SCALAR_GT; break;
                    case K::GreaterEqual: opcode = ORES_SCALAR_GE; break;
                    default: deny("unsupported scalar instruction emission");
                }
                emit(opcode);
                return;
            }
        }
    }

    void terminal(const Expression& e, const Function& fn,
                  const std::unordered_map<std::string, std::size_t>& locals) {
        if (e.kind == Expression::Kind::Call) {
            // A direct return-call has no pending arithmetic or cleanup and
            // therefore reuses the current actor-owned frame (proper TCO).
            expression(*e.arguments[0], fn, locals);
            emit(ORES_SCALAR_TAIL_CALL, (std::int64_t)ids.at(e.name));
        } else {
            expression(e, fn, locals);
            emit(ORES_SCALAR_RETURN);
        }
    }

    void guard(const Expression& condition, const Expression& result,
               const Function& fn,
               const std::unordered_map<std::string, std::size_t>& locals) {
        expression(condition, fn, locals);
        const std::size_t jump = out.code.size();
        emit(ORES_SCALAR_BRANCH_FALSE, 0);
        terminal(result, fn, locals);
        out.code[jump].immediate = (std::int64_t)out.code.size();
    }

    void function(const Function& fn) {
        out.entries.push_back(out.code.size());
        std::unordered_map<std::string, std::size_t> locals;
        for (const auto& local : fn.locals) {
            expression(*local.initializer, fn, locals);
            const auto slot = locals.size();
            if (local.name == fn.parameters[0].name
                || !locals.emplace(local.name, slot).second)
                deny("scalar frame lexical local shadowing");
            emit(ORES_SCALAR_STORE_LOCAL, (std::int64_t)slot);
        }
        for (const auto& statement : fn.statements)
            guard(*statement.condition,
                  *statement.then_statements[0].expression, fn, locals);
        for (const auto& g : fn.early_returns)
            guard(*g.condition, *g.result, fn, locals);
        terminal(*fn.result, fn, locals);
    }

    ActorScalarCompiledProgram compile(const std::string& root) {
        out.root = enroll(root);
        // All reachable callables are validated before the first instruction
        // is emitted: no partial graph can fall back to native recursion.
        for (std::size_t i = 0; i < funcs.size(); ++i)
            validate_function(*funcs[i]);
        for (const Function* fn : funcs)
            function(*fn);
        if (out.code.size() > ORES_SCALAR_MAX_INSTRUCTIONS)
            deny("compiled scalar code exceeds per-actor quota");
        return std::move(out);
    }
};
} // namespace actor_scalar_m3

inline std::optional<ActorScalarCompiledProgram>
compile_actor_scalar_graph(const Program& program, const ActorDecl& actor) {
    const Function& handler = actor.on_message;
    bool any_call = handler.result &&
        actor_scalar_m3::contains_call(*handler.result);
    for (const auto& local : handler.locals)
        if (local.initializer &&
            actor_scalar_m3::contains_call(*local.initializer))
            any_call = true;
    for (const auto& write : handler.actor_field_writes)
        if (write.expression &&
            actor_scalar_m3::contains_call(*write.expression))
            any_call = true;
    if (!any_call) return std::nullopt;
    if (!handler.result ||
        handler.result->kind != Expression::Kind::Call ||
        handler.result->arguments.size() != 1 ||
        handler.result->type_arguments_present ||
        (!handler.result->argument_names.empty() &&
            (handler.result->argument_names.size() != 1 ||
             !handler.result->argument_names[0].empty())) ||
        !handler.locals.empty() || !handler.actor_field_writes.empty() ||
        !handler.result->arguments[0] ||
        handler.result->arguments[0]->kind != Expression::Kind::Variable ||
        handler.result->arguments[0]->name !=
            handler.parameters.at(0).name)
        actor_scalar_m3::deny(
            "actor callback may only directly return pure unary fnc(message)");
    actor_scalar_m3::Builder builder(program);
    return builder.compile(handler.result->name);
}
} // namespace oreslang
