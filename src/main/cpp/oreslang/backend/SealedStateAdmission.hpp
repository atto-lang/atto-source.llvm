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
    for (const auto& actor : actors) {
        // A parser-bypassing AST must not route a sealed-state capability
        // through the legacy handler, whose return is dereferenced directly.
        if (!actor.sealed_state) {
            const auto& legacy = actor.on_message;
            if (legacy.actor_state_loan || legacy.parameters.size() != 1 ||
                    !legacy.result || legacy.returns_void)
                throw std::runtime_error("legacy actor contains invalid sealed-state authority or return");
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
        for (const auto& write : loan->writes) {
            if (!field_names.count(write.field) || !write.expression)
                throw std::runtime_error("sealed actor state write references an invalid field");
        }
    }
}

} // namespace oreslang
