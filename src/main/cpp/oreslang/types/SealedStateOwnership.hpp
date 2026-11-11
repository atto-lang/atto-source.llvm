#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace oreslang {

// Independent compiler-local revalidation of the normalized scalar critical
// scope. Parser flags alone must not authorize field mutation or erase effects.
// This is not a general actor/shared-region loan solver or a Rust adapter.
inline void validate_sealed_actor_ownership(const ActorDecl& actor) {
    const auto deny = [](const char* reason) {
        throw std::runtime_error(std::string("sealed state ownership: ") + reason);
    };
    if (!actor.has_state_schema) {
        if (actor.sealed_state_synchronous_v1 || actor.sealed_state_write_v1)
            deny("proof without schema");
        return;
    }
    const auto& fn = actor.on_message;
    if (!actor.isolated || !actor.sealed_state_synchronous_v1 ||
        actor.state_schema.empty() || actor.state_schema.size() > 16 ||
        actor.state_schema.size() != actor.fields.size() ||
        fn.name != "on_message" || fn.parameters.size() != 1 || !fn.result)
        deny("synchronous scope and exact schema required");
    const auto& parameter = fn.parameters.front();
    if (parameter.mode != Parameter::Mode::Value || parameter.scalar_type != ScalarType::I64 ||
        parameter.name.empty() || !parameter.class_name.empty() || !parameter.sum_type.empty() ||
        !parameter.generic_type.empty() || !parameter.pending_type_alias.empty() ||
        fn.returns_void || fn.return_type != ScalarType::I64 || !fn.return_class.empty() ||
        !fn.return_sum.empty() || !fn.pending_return_type_alias.empty())
        deny("only value i64 input/result admitted");
    if (!fn.locals.empty() || !fn.statements.empty() || !fn.early_returns.empty() ||
        !fn.generic_parameters.empty() || fn.scalar_while || !fn.actor_spawns.empty() ||
        !fn.source_tasks.empty() || !fn.future_pipelines.empty() || fn.async_source_i64 ||
        !fn.ready_await_actor.empty() || !fn.done_await_actor.empty() ||
        fn.ready_await_form != NativeAwaitForm::None || !fn.ready_future_name.empty() ||
        !fn.ready_future_actor.empty() || !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
        !fn.actor_actions.empty() || fn.caught_throw_value || !fn.catch_binding.empty() ||
        !fn.caught_throw_defers.empty() || !fn.method_owner.empty() || fn.native_internal ||
        !fn.c_export_u32_symbol.empty() || fn.has_declared_effects)
        deny("unmodeled body, suspension, escape or exceptional effect");

    std::unordered_set<std::string> names{parameter.name};
    std::unordered_set<std::string> fields;
    for (std::size_t i = 0; i < actor.state_schema.size(); ++i) {
        const auto& schema = actor.state_schema[i];
        const auto& slot = actor.fields[i];
        if (schema.name.empty() || !names.insert(schema.name).second ||
            schema.type != ScalarType::I64 || schema.name != slot.name ||
            slot.scalar_type != ScalarType::I64 || slot.int_array || slot.heap_owned ||
            !slot.class_name.empty() || slot.new_class || slot.record_view || slot.move_class ||
            !slot.constructor_arguments.empty() || !slot.move_source.empty() ||
            !slot.array_record_class.empty() || !slot.pending_type_alias.empty() ||
            !slot.sum_type.empty() || !slot.task_await_name.empty() ||
            !slot.future_await_name.empty() || slot.owner_mutable || slot.assignable ||
            slot.field_mutable != actor.sealed_state_write_v1 || !slot.initializer ||
            slot.initializer->kind != Expression::Kind::Integer ||
            slot.initializer->integer != schema.initial_i64)
            deny("layout or access mode differs from sealed schema");
        fields.insert(schema.name);
    }
    if (fn.actor_field_writes.size() > 1 ||
        (!actor.sealed_state_write_v1 && !fn.actor_field_writes.empty()))
        deny("read mutation or unproven multi-field commit");

    std::size_t remaining = 4096;
    std::function<void(const Expression&, unsigned)> scalar;
    scalar = [&](const Expression& expression, unsigned depth) {
        using K = Expression::Kind;
        if (depth > 128 || remaining == 0) deny("scalar expression budget exceeded");
        --remaining;
        if (expression.resolved_type != ScalarType::I64 || !expression.resolved_class.empty() ||
            !expression.sum_type.empty() || !expression.arguments.empty() ||
            !expression.argument_names.empty() || !expression.type_arguments.empty() ||
            expression.type_arguments_present || expression.condition ||
            expression.condition_lhs || expression.condition_rhs ||
            expression.when_true || expression.when_false)
            deny("hidden reference, call or control effect");
        switch (expression.kind) {
            case K::Integer:
                if (expression.left || expression.right) deny("unexpected literal operands");
                return;
            case K::Variable:
                if (!names.count(expression.name) || expression.left || expression.right)
                    deny("unknown state/value alias");
                return;
            case K::Negate:
                if (!expression.left || expression.right) deny("invalid unary expression");
                scalar(*expression.left, depth + 1);
                return;
            case K::Add: case K::Subtract: case K::Multiply:
                if (!expression.left || !expression.right) deny("invalid binary expression");
                scalar(*expression.left, depth + 1);
                scalar(*expression.right, depth + 1);
                return;
            default: deny("non-scalar/borrow/call escape in critical scope");
        }
    };
    for (const auto& write : fn.actor_field_writes) {
        if (!fields.count(write.field) || !write.expression) deny("unknown write target");
        scalar(*write.expression, 0);
    }
    scalar(*fn.result, 0);
    if (!fn.actor_field_writes.empty() && fn.result->kind != Expression::Kind::Variable &&
        fn.result->kind != Expression::Kind::Integer)
        deny("postcommit result may trap");
}

} // namespace oreslang
