#pragma once

#include "oreslang/backend/ActorRefOwnershipVerifier.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace oreslang {

/*
 * Production source/HIR authority for actor-local managed references.
 *
 * This class is the only non-test friend allowed to construct
 * ActorRefOwnershipEvidence. Callers select a parsed actor/state field and
 * requested access, but cannot supply MIR claims, CFGs, "verified" booleans,
 * origins, escape classifications, or physical ref widths.
 *
 * Current slice is intentionally narrow:
 *  - canonical actor state { ... }
 *  - Ref<int> state fields only
 *  - exact state.<ref>.value use inside one lexical critical handle_state loan
 *  - no suspension, FFI, mailbox transport, return of the Ref itself, or casts
 *  - compact32 only on native64 with caller-supplied bounded actor arena size
 *
 * Allocation/publication lowering is still a separate backend integration gate.
 */
class SourceOwnershipChecker final {
public:
    static backend::ActorRefOwnershipEvidence issue_actor_state_ref_evidence(
            const Program& program,
            const std::string& actor_name,
            const std::string& state_field,
            backend::ManagedRefAccess access,
            std::uint64_t actor_region_capacity_bytes,
            bool native64_target) {
        const auto actor_it = std::find_if(program.actors.begin(), program.actors.end(),
            [&](const ActorDecl& actor) { return actor.name == actor_name; });
        if (actor_it == program.actors.end())
            throw std::invalid_argument("source ownership checker: unknown actor");
        const ActorDecl& actor = *actor_it;
        if (!actor.state_declared || !actor.on_message.actor_state_loan)
            throw std::invalid_argument(
                "source ownership checker: compact Ref requires canonical actor state loan");

        const auto field_it = std::find_if(actor.fields.begin(), actor.fields.end(),
            [&](const LocalBinding& field) { return field.name == state_field; });
        if (field_it == actor.fields.end() || !field_it->managed_ref ||
                field_it->managed_ref_pointee != "int")
            throw std::invalid_argument(
                "source ownership checker: compact Ref requires declared Ref<int> state field");

        const ActorStateLoan& loan = *actor.on_message.actor_state_loan;
        if (!loan.critical)
            throw std::invalid_argument(
                "source ownership checker: compact Ref requires critical handle_state loan");
        if (access == backend::ManagedRefAccess::Write &&
                loan.mode != ActorStateLoan::Mode::Write)
            throw std::invalid_argument(
                "source ownership checker: compact Ref write requires mutable state loan");
        if (access == backend::ManagedRefAccess::Read &&
                loan.mode != ActorStateLoan::Mode::Read)
            throw std::invalid_argument(
                "source ownership checker: read evidence requires critical read loan");

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

        ownership::Graph graph;
        const std::string place = "state." + state_field;
        graph.entry.emplace(place, ownership::PlaceState{ownership::kOwned});

        ownership::BasicBlock acquire;
        acquire.operations.emplace_back(
            access == backend::ManagedRefAccess::Write
                ? ownership::Operation::Kind::BorrowExclusive
                : ownership::Operation::Kind::BorrowShared,
            place, UINT64_C(1));
        acquire.successors.push_back(1);

        ownership::BasicBlock exact_site;
        exact_site.successors.push_back(2);

        ownership::BasicBlock release;
        release.operations.emplace_back(
            access == backend::ManagedRefAccess::Write
                ? ownership::Operation::Kind::ReleaseExclusive
                : ownership::Operation::Kind::ReleaseShared,
            place, UINT64_C(1));

        graph.blocks.push_back(std::move(acquire));
        graph.blocks.push_back(std::move(exact_site));
        graph.blocks.push_back(std::move(release));

        const auto claims =
            backend::ActorRefOwnershipVerifier::derive_verified_state_loan_claims(
                graph, 1, place, callback, actor.on_message.declared_effects,
                access, actor_region_capacity_bytes, native64_target);

        // Private constructor: only this production source/HIR authority can
        // cross from revalidated source facts into codegen evidence.
        return backend::ActorRefOwnershipEvidence(claims);
    }
};

} // namespace oreslang
