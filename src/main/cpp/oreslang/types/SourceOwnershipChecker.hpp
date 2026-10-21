#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/CriticalHandleStateContract.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang {

/*
 * Non-authorizing proof that parsed source contains one exact, lexically
 * admitted managed-reference payload use inside the actor's sealed critical
 * state loan.
 *
 * IMPORTANT: this is deliberately NOT ActorRefOwnershipEvidence. Source syntax
 * alone cannot prove that a concrete actor-private allocation was published,
 * that its physical slot is quarantined, or which backend storage width was
 * chosen. Those facts only exist after allocation/publication lowering.
 */
class SourceManagedRefUseProof final {
    std::string actor_;
    std::string state_field_;
    backend::ManagedRefAccess access_;

    SourceManagedRefUseProof(std::string actor, std::string state_field,
                             backend::ManagedRefAccess access)
        : actor_(std::move(actor)), state_field_(std::move(state_field)),
          access_(access) {}
    friend class SourceOwnershipChecker;
public:
    SourceManagedRefUseProof() = delete;
    const std::string& actor() const noexcept { return actor_; }
    const std::string& state_field() const noexcept { return state_field_; }
    backend::ManagedRefAccess access() const noexcept { return access_; }
};

/*
 * Production source/HIR checker for logical actor-local managed references.
 *
 * The checker reparses raw source inside this authority boundary. Callers
 * cannot supply a mutated Program/HIR, ownership CFG, provenance flags,
 * physical storage width, actor-region size, or a "verified" boolean.
 *
 * This milestone proves source use/loan authority only. It intentionally does
 * not mint ActorRefOwnershipEvidence until source allocation + publication
 * lowering can bind a real ActorArena allocation to the exact use site.
 */
class SourceOwnershipChecker final {
public:
    static SourceManagedRefUseProof validate_actor_state_ref_use(
            std::string_view source,
            const std::string& actor_name,
            const std::string& state_field,
            backend::ManagedRefAccess access) {
        Program program = parse_program(source);
        const auto actor_it = std::find_if(program.actors.begin(), program.actors.end(),
            [&](const ActorDecl& actor) { return actor.name == actor_name; });
        if (actor_it == program.actors.end())
            throw std::invalid_argument("source ownership checker: unknown actor");
        const ActorDecl& actor = *actor_it;
        if (!actor.state_declared || !actor.on_message.actor_state_loan)
            throw std::invalid_argument(
                "source ownership checker: managed Ref requires canonical actor state loan");

        const auto field_it = std::find_if(actor.fields.begin(), actor.fields.end(),
            [&](const LocalBinding& field) { return field.name == state_field; });
        if (field_it == actor.fields.end() || !field_it->managed_ref ||
                field_it->managed_ref_pointee != "int")
            throw std::invalid_argument(
                "source ownership checker: managed Ref requires declared Ref<int> state field");

        const ActorStateLoan& loan = *actor.on_message.actor_state_loan;
        if (!loan.critical)
            throw std::invalid_argument(
                "source ownership checker: managed Ref requires critical handle_state loan");
        if (access == backend::ManagedRefAccess::Write &&
                loan.mode != ActorStateLoan::Mode::Write)
            throw std::invalid_argument(
                "source ownership checker: managed Ref write requires mutable state loan");
        if (access == backend::ManagedRefAccess::Read &&
                loan.mode != ActorStateLoan::Mode::Read)
            throw std::invalid_argument(
                "source ownership checker: managed Ref read requires critical read loan");

        const auto is_payload_read = [&](const Expression& root) {
            bool found = false;
            std::function<void(const Expression&)> visit;
            visit = [&](const Expression& expr) {
                if (expr.kind == Expression::Kind::Field && expr.name == "value" &&
                        expr.left && expr.left->kind == Expression::Kind::Field) {
                    const Expression& state_ref = *expr.left;
                    if (state_ref.name == state_field && state_ref.left &&
                            state_ref.left->kind == Expression::Kind::Variable &&
                            state_ref.left->name == "state") {
                        found = true;
                        return;
                    }
                }
                if (expr.left) visit(*expr.left);
                if (expr.right) visit(*expr.right);
                for (const auto& arg : expr.arguments) visit(*arg);
            };
            visit(root);
            return found;
        };

        bool exact_use = false;
        if (access == backend::ManagedRefAccess::Write) {
            exact_use = std::any_of(loan.writes.begin(), loan.writes.end(),
                [&](const ActorFieldWrite& write) {
                    return write.managed_ref_payload && write.field == state_field;
                });
        } else {
            exact_use = loan.result && is_payload_read(*loan.result);
            if (!exact_use) {
                exact_use = std::any_of(loan.writes.begin(), loan.writes.end(),
                    [&](const ActorFieldWrite& write) {
                        return write.expression && is_payload_read(*write.expression);
                    });
            }
        }
        if (!exact_use)
            throw std::invalid_argument(
                "source ownership checker: no exact managed Ref payload use in active loan");

        const FunctionEffect required = access == backend::ManagedRefAccess::Write
            ? FunctionEffect::StateWrite : FunctionEffect::StateRead;
        if (!actor.on_message.has_declared_effects ||
                !actor.on_message.declared_effects.has(required))
            throw std::invalid_argument(
                "source ownership checker: declared effects do not authorize Ref access");

        CriticalStateCallback callback{};
        callback.intrinsic_critical = true;
        callback.callback_critical = true;
        callback.lexical = true;
        callback.one_state_parameter = true;
        callback.escapes = false;
        callback.state_loan_live_across_suspend = false;
        callback.continuation_generation_fenced = false;
        callback.inferred.bits = static_cast<std::uint32_t>(required);
        if (admit_critical_handle_state(
                callback, actor.on_message.declared_effects, false)
                != CriticalStateAdmission::Accept)
            throw std::invalid_argument(
                "source ownership checker: state callback admission rejected");

        return SourceManagedRefUseProof(actor_name, state_field, access);
    }
};

} // namespace oreslang
