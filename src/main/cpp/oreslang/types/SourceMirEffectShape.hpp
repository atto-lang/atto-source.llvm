#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oreslang::source_identity {

// In-process structural change detector for the deliberately small
// straight-line source-MIR domain. This is NOT a cryptographic signature or
// proof against arbitrary native memory corruption. An unsupported / enormous
// source returns nullopt and can never acquire a scoped completeness claim.
//
// Deliberately enumerated independently of LLVM and the typed MIR builder.
// Includes source expressions and class layouts, neither of which are
// protected by Statement source-node ordinal/path identities alone.
class SourceEffectShape final {
public:
    void u64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            state_ ^= static_cast<std::uint8_t>(value);
            state_ *= UINT64_C(1099511628211);
            value >>= 8;
        }
    }
    void str(std::string_view value) {
        u64(value.size());
        for (unsigned char ch : value) {
            state_ ^= ch;
            state_ *= UINT64_C(1099511628211);
        }
    }
    std::uint64_t value() const { return state_ ? state_ : 1; }
private:
    std::uint64_t state_ = UINT64_C(1469598103934665603);
};

inline std::optional<std::uint64_t> straight_line_effect_shape(
        const Program& program, const Function& function) {
    // Never hash enormous or noncandidate functions merely to satisfy an
    // optional source-MIR optimization. The compiler can continue without
    // claiming straight-line completeness.
    if (!function.result || function.locals.empty() ||
            function.locals.size() > 64 || function.statements.size() > 128 ||
            program.classes.size() > 64)
        return std::nullopt;
    bool has_class = false;
    for (const auto& local : function.locals)
        has_class = has_class || !local.class_name.empty();
    if (!has_class)
        return std::nullopt;

    SourceEffectShape hash;
    hash.str("atto/straight-line/source-effect-shape/v1");
    std::size_t total_nodes = 0;
    constexpr std::size_t kMaxNodes = 4096;
    constexpr std::size_t kMaxDepth = 48;
    bool valid = true;

    const auto expression = [&](const auto& self,
            const Expression* expr, std::size_t depth) -> void {
        if (!expr) {
            hash.u64(0);
            return;
        }
        if (++total_nodes > kMaxNodes || depth > kMaxDepth) {
            valid = false;
            return;
        }
        hash.u64(1);
        hash.u64(static_cast<std::uint64_t>(expr->kind));
        hash.u64(expr->offset);
        hash.u64(static_cast<std::uint64_t>(expr->integer));
        hash.u64(expr->boolean ? 1 : 0);
        hash.u64(static_cast<std::uint64_t>(expr->resolved_type));
        hash.str(expr->resolved_class);
        hash.str(expr->name);
        hash.str(expr->sum_type);
        self(self, expr->left.get(), depth + 1);
        self(self, expr->right.get(), depth + 1);
        self(self, expr->condition.get(), depth + 1);
        self(self, expr->condition_lhs.get(), depth + 1);
        self(self, expr->condition_rhs.get(), depth + 1);
        self(self, expr->when_true.get(), depth + 1);
        self(self, expr->when_false.get(), depth + 1);
        hash.u64(expr->arguments.size());
        for (const auto& item : expr->arguments)
            self(self, item.get(), depth + 1);
        hash.u64(expr->argument_names.size());
        for (const auto& name : expr->argument_names) hash.str(name);
        hash.u64(expr->type_arguments_present ? 1 : 0);
        hash.u64(expr->type_arguments.size());
        for (const auto& arg : expr->type_arguments) {
            hash.str(arg.name);
            hash.str(arg.type);
            hash.u64(arg.infer ? 1 : 0);
        }
    };

    // The class declaration is part of the ownership-relevant source:
    // changing a field initializer, type, order, finality or layout after
    // compiler admission must invalidate a previously sealed effect trace.
    hash.u64(program.classes.size());
    for (const auto& cls : program.classes) {
        hash.str(cls.name);
        hash.u64(cls.exported ? 1 : 0);
        hash.u64(cls.explicit_constructor ? 1 : 0);
        hash.u64(cls.fields.size());
        for (const auto& field : cls.fields) {
            hash.str(field.name);
            hash.u64(static_cast<std::uint64_t>(field.initial));
            hash.u64(static_cast<std::uint64_t>(field.scalar_type));
            hash.u64(static_cast<std::uint64_t>(field.array_element_type));
            hash.str(field.class_name);
            hash.str(field.array_record_class);
            hash.str(field.sum_type);
            hash.u64(field.public_member ? 1 : 0);
            hash.u64(field.int_array ? 1 : 0);
            hash.u64(field.final_field ? 1 : 0);
            hash.u64(field.mutable_referent ? 1 : 0);
            hash.u64(field.constructor_argument ? 1 : 0);
            expression(expression, field.initializer_expression.get(), 0);
        }
        hash.u64(cls.constructor_parameters.size());
        for (const auto& name : cls.constructor_parameters) hash.str(name);
        hash.u64(cls.constructor_parameter_types.size());
        for (auto ty : cls.constructor_parameter_types)
            hash.u64(static_cast<std::uint64_t>(ty));
        hash.u64(cls.methods.size());
        for (const auto& method : cls.methods) {
            hash.str(method.name);
            hash.u64(method.arity);
            hash.u64(method.public_member ? 1 : 0);
            hash.u64(method.mutable_receiver ? 1 : 0);
        }
    }

    hash.str(function.name);
    hash.u64(static_cast<std::uint64_t>(function.return_type));
    hash.u64(function.returns_void ? 1 : 0);
    hash.str(function.return_class);
    hash.u64(function.locals.size());
    for (const auto& local : function.locals) {
        hash.str(local.name);
        hash.str(local.class_name);
        hash.str(local.pending_type_alias);
        hash.str(local.sum_type);
        hash.str(local.array_record_class);
        hash.str(local.task_await_name);
        hash.str(local.future_await_name);
        hash.str(local.move_source);
        hash.u64(local.offset);
        hash.u64(static_cast<std::uint64_t>(local.scalar_type));
        hash.u64(static_cast<std::uint64_t>(local.array_element_type));
        hash.u64(local.new_class ? 1 : 0);
        hash.u64(local.heap_owned ? 1 : 0);
        hash.u64(local.int_array ? 1 : 0);
        hash.u64(local.record_view ? 1 : 0);
        hash.u64(local.move_class ? 1 : 0);
        hash.u64(local.owner_mutable ? 1 : 0);
        hash.u64(local.field_mutable ? 1 : 0);
        hash.u64(local.assignable ? 1 : 0);
        expression(expression, local.initializer.get(), 0);
        hash.u64(local.constructor_arguments.size());
        for (const auto& argument : local.constructor_arguments)
            expression(expression, argument.get(), 0);
    }
    hash.u64(function.statements.size());
    for (const auto& stmt : function.statements) {
        hash.u64(static_cast<std::uint64_t>(stmt.kind));
        hash.str(stmt.receiver);
        hash.str(stmt.field);
        hash.u64(stmt.offset);
        expression(expression, stmt.expression.get(), 0);
        expression(expression, stmt.condition.get(), 0);
        expression(expression, stmt.condition_lhs.get(), 0);
        expression(expression, stmt.condition_rhs.get(), 0);
        hash.u64(stmt.then_statements.size());
        hash.u64(stmt.else_statements.size());
        hash.u64(stmt.branch_local ? 1 : 0);
        hash.u64(stmt.loop_step ? 1 : 0);
        hash.u64(stmt.array_commit_values.size());
    }
    expression(expression, function.result.get(), 0);
    hash.u64(function.early_returns.size());
    hash.u64(function.caught_throw_defers.size());
    hash.u64(function.actor_spawns.size());
    hash.u64(function.actor_actions.size());
    hash.u64(function.actor_sends.size());
    hash.u64(function.actor_stops.size());
    hash.u64(function.actor_field_writes.size());
    hash.u64(function.source_tasks.size());
    hash.u64(function.future_pipelines.size());
    hash.u64(function.scalar_while ? 1 : 0);
    hash.u64(function.caught_throw_value ? 1 : 0);
    hash.u64(function.ready_await_form == NativeAwaitForm::None ? 0 : 1);
    hash.str(function.ready_await_actor);
    hash.str(function.done_await_actor);
    hash.u64(function.async_source_i64 ? 1 : 0);

    if (!valid)
        return std::nullopt;
    return hash.value();
}

} // namespace oreslang::source_identity
