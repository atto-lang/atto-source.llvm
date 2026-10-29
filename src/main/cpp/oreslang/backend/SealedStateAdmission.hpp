#pragma once

#include "oreslang/types/FunctionEffects.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace oreslang {

// This check runs before LLVM IR creation. It deliberately accepts an actor
// container rather than an LLVM object, so the same admission boundary can be
// exercised without a native LLVM SDK or codegen mocks.
template <typename ActorContainer>
void verify_sealed_actor_state_shape(const ActorContainer& actors) {
    // Both actor modes lower exactly one VALUE i64 message parameter.
    // Compiler-built ASTs can bypass parser checks on aliases/borrow modes.
    const auto plain_message = [](const auto& param) {
        return !param.name.empty() &&
               param.scalar_type == decltype(param.scalar_type)::I64 &&
               param.mode == decltype(param.mode)::Value &&
               param.class_name.empty() && param.generic_type.empty() &&
               param.pending_type_alias.empty() && param.sum_type.empty();
    };
    // An actor field is physically one integer heap slot; do not allow
    // forged type aliases, array element types, or ownership metadata to
    // turn it into a reference while lowering it as a scalar.
    const auto plain_slot = [](const auto& field) {
        return field.scalar_type == decltype(field.scalar_type)::I64 &&
               field.array_element_type == decltype(field.array_element_type)::I64 &&
               !field.heap_owned && !field.int_array && !field.record_view &&
               field.array_record_class.empty() && field.class_name.empty() &&
               !field.new_class && field.constructor_arguments.empty() &&
               !field.move_class && field.move_source.empty() &&
               !field.owner_mutable && field.pending_type_alias.empty() &&
               field.sum_type.empty();
    };
    std::unordered_set<std::string> actor_names;
    for (const auto& actor : actors) {
        // Actor codegen interns symbols and handlers by nominal name. A
        // directly constructed AST must not alias two actors into one entry.
        if (actor.name.empty() || !actor_names.insert(actor.name).second)
            throw std::runtime_error("actor nominal name is empty or duplicated");
        if (actor.on_message.parameters.size() != 1 ||
                !plain_message(actor.on_message.parameters.front()))
            throw std::runtime_error("actor handler must take exactly one plain i64 value");
        // A parser-bypassing AST must not route a sealed-state capability
        // through the legacy handler, whose return is dereferenced directly.
        if (!actor.sealed_state) {
            const auto& legacy = actor.on_message;
            if (legacy.actor_state_loan || legacy.parameters.size() != 1 ||
                    !legacy.result || legacy.returns_void ||
                    legacy.name != "on_message" || !legacy.exported ||
                    legacy.return_type != decltype(legacy.return_type)::I64 ||
                    legacy.parameters.front().name.empty())
                throw std::runtime_error("legacy actor contains invalid handler authority or return");
            // Directly constructed legacy ASTs bypass source validation. Check
            // all fields before codegen dereferences initializers or aliases a
            // message stack slot through an ambiguous field name.
            if (actor.fields.empty() || actor.fields.size() > 16)
                throw std::runtime_error("legacy actor requires bounded scalar fields");
            std::unordered_set<std::string> legacy_names;
            for (const auto& field : actor.fields) {
                if (field.name.empty() || field.name == "state" ||
                        field.name == legacy.parameters.front().name ||
                        !legacy_names.insert(field.name).second ||
                        !field.initializer || !plain_slot(field))
                    throw std::runtime_error("legacy actor contains an invalid state field");
            }
            continue;
        }
        const auto& method = actor.on_message;
        if (method.name != "on_message" || !method.exported ||
                method.returns_void ||
                method.return_type != decltype(method.return_type)::I64 ||
                method.parameters.size() != 1 ||
                method.parameters.front().name.empty())
            throw std::runtime_error("sealed actor requires one public int message handler");
        const auto& message = method.parameters.front().name;
        if (actor.fields.size() > 16)
            throw std::runtime_error("sealed actor state exceeds field bound");
        std::unordered_set<std::string> field_names;
        for (const auto& field : actor.fields) {
            if (field.name.empty() || field.name == "state" ||
                    field.name == message || !field_names.insert(field.name).second)
                throw std::runtime_error("sealed actor state has ambiguous or reserved field");
            if (!field.initializer || !field.field_mutable)
                throw std::runtime_error("sealed actor state field is not initialized and writable");
            // A canonical native state slot is currently a plain i64 value.
            // Forged owner/record/array metadata cannot reach heap lowering.
            if (!plain_slot(field))
                throw std::runtime_error("sealed actor state forbids non-scalar storage metadata");
        }
        const auto* loan = method.actor_state_loan.get();
        if (!loan || loan->binder.empty() || loan->binder == message ||
                !loan->result || !method.has_declared_effects ||
                !method.declared_effects.valid())
            throw std::runtime_error("sealed actor state lacks valid lexical authority");
        if (loan->mode != decltype(loan->mode)::Read &&
                loan->mode != decltype(loan->mode)::Write)
            throw std::runtime_error("invalid sealed actor state loan mode");
        if (loan->mode == decltype(loan->mode)::Read) {
            if (!method.declared_effects.has(FunctionEffect::StateRead) ||
                    !loan->writes.empty())
                throw std::runtime_error("sealed actor state read lacks read-only authority");
        } else if (!method.declared_effects.has(FunctionEffect::StateWrite)) {
            throw std::runtime_error("sealed actor state write lacks state.write authority");
        }
        if (method.result || !method.actor_field_writes.empty() ||
                !method.locals.empty() || !method.actor_sends.empty() ||
                !method.actor_stops.empty() || !method.actor_actions.empty() ||
                !method.statements.empty() || !method.early_returns.empty() ||
                !method.actor_spawns.empty())
            throw std::runtime_error("sealed actor state mixed with legacy handler storage");
        // These are alternate compiler-lowering lanes, not benign metadata:
        // none is admitted by the one-lexical-callback actor grammar.
        if (!method.generic_parameters.empty() || method.scalar_while ||
                !method.ready_await_actor.empty() || !method.done_await_actor.empty() ||
                method.ready_await_form != decltype(method.ready_await_form)::None ||
                !method.ready_future_name.empty() || !method.ready_future_actor.empty() ||
                method.caught_throw_value || !method.caught_throw_defers.empty() ||
                !method.catch_binding.empty() || !method.return_class.empty() ||
                !method.return_sum.empty() || !method.pending_return_type_alias.empty() ||
                !method.method_owner.empty() || method.native_internal)
            throw std::runtime_error("sealed actor state cannot carry lifecycle or alternate lowering");
        // Compiler passes can construct a sealed callback AST without
        // going through the parser's lexical owned-local checks. Reject
        // non-i64, mutable, forwarding, or escaped ownership metadata before
        // any native IR allocation or heap slot lowering.
        if (loan->locals.size() > 8)
            throw std::runtime_error("sealed callback owned-local bound exceeded");
        std::unordered_set<std::string> owned_names;
        for (const auto& local : loan->locals) {
            if (local.name.empty() || local.name == message ||
                    local.name == loan->binder ||
                    field_names.count(local.name) ||
                    !owned_names.insert(local.name).second ||
                    !local.heap_owned || !local.initializer ||
                    local.scalar_type != decltype(local.scalar_type)::I64 ||
                    local.array_element_type != decltype(local.array_element_type)::I64 ||
                    local.owner_mutable || local.assignable ||
                    local.field_mutable || local.int_array || local.record_view ||
                    local.new_class || local.move_class ||
                    !local.class_name.empty() ||
                    !local.array_record_class.empty() ||
                    !local.move_source.empty() ||
                    !local.constructor_arguments.empty() ||
                    !local.pending_type_alias.empty() ||
                    !local.sum_type.empty() ||
                    !local.task_await_name.empty() ||
                    !local.future_await_name.empty())
                throw std::runtime_error(
                    "sealed callback rejects forged owned-local provenance");
        }
        for (const auto& write : loan->writes) {
            if (!field_names.count(write.field) || !write.expression)
                throw std::runtime_error("sealed actor state write references an invalid field");
        }
    }
}

} // namespace oreslang
