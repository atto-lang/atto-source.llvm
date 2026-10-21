#pragma once

#include "oreslang/backend/ActorRefOwnershipVerifier.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace oreslang {

/*
 * Source/HIR authority for actor-local managed-reference ownership.
 *
 * Unlike ActorRefOwnershipVerifier, callers cannot provide an ownership CFG,
 * callback facts, provenance booleans, or MIR claims. This pass reconstructs
 * those facts from the compiler's parsed actor/state-loan AST and then asks the
 * backend policy helper to validate the derived graph.
 *
 * This milestone deliberately returns candidate MIR claims only. The parser
 * still has no canonical source Ref<T> allocation/use node, so there is not yet
 * an exact source reference site to bind ActorRefOwnershipEvidence to. The
 * private evidence constructor remains reserved for this class; issuance stays
 * disabled until a real logical Ref<T> HIR node exists.
 */
class SourceOwnershipChecker final {
public:
    SourceOwnershipChecker() = delete;

    static backend::ActorManagedRefMir derive_actor_state_ref_claims(
            const Program& program,
            std::string_view actor_name,
            backend::ManagedRefAccess access,
            std::uint64_t actor_region_capacity_bytes,
            bool native64_target) {
        const ActorDecl* actor = nullptr;
        for (const auto& candidate : program.actors) {
            if (candidate.name != actor_name) continue;
            if (actor)
                throw std::invalid_argument(
                    "source ownership requires unique actor declaration");
            actor = &candidate;
        }
        if (!actor)
            throw std::invalid_argument(
                "source ownership requires known actor declaration");
        if (!actor->state_declared || actor->fields.empty())
            throw std::invalid_argument(
                "managed actor reference requires canonical state declaration");

        const Function& method = actor->on_message;
        if (method.parameters.size() != 1 || !method.actor_state_loan)
            throw std::invalid_argument(
                "managed actor reference requires sealed handle_state source loan");
        if (!method.locals.empty() || !method.statements.empty() ||
                !method.actor_field_writes.empty() || !method.early_returns.empty() ||
                !method.actor_spawns.empty() || !method.actor_sends.empty() ||
                !method.actor_stops.empty())
            throw std::invalid_argument(
                "managed actor reference source loan contains unsupported side effects");

        const ActorStateLoan& loan = *method.actor_state_loan;
        if (!loan.critical)
            throw std::invalid_argument(
                "compact managed reference requires explicit critical state callback");

        const auto expected_mode = access == backend::ManagedRefAccess::Write
            ? ActorStateLoan::Mode::Write : ActorStateLoan::Mode::Read;
        if (loan.mode != expected_mode)
            throw std::invalid_argument(
                "managed reference access must match source state-loan mode");

        const FunctionEffect required_effect =
            access == backend::ManagedRefAccess::Write
                ? FunctionEffect::StateWrite
                : FunctionEffect::StateRead;
        if (!method.has_declared_effects ||
                !method.declared_effects.has(required_effect))
            throw std::invalid_argument(
                "source state loan lacks matching declared ownership effect");
        if (method.declared_effects.has(FunctionEffect::Suspend) ||
                method.declared_effects.has(FunctionEffect::Ffi))
            throw std::invalid_argument(
                "compact managed reference source loan cannot declare suspend or FFI");

        std::unordered_set<std::string> state_fields;
        for (const auto& field : actor->fields) {
            if (field.name.empty() || !field.initializer ||
                    !state_fields.insert(field.name).second)
                throw std::invalid_argument(
                    "source ownership requires unique initialized state fields");
        }

        const std::string& parameter = method.parameters.front().name;
        if (parameter.empty() || state_fields.count(parameter))
            throw std::invalid_argument(
                "source ownership requires unshadowed message parameter");
        if (loan.result_binding.empty() ||
                loan.result_binding == parameter ||
                state_fields.count(loan.result_binding))
            throw std::invalid_argument(
                "source ownership requires isolated copied loan result");

        const auto validate_callback_expression =
            [&](const Expression& root) {
                std::function<void(const Expression&, std::size_t)> visit;
                visit = [&](const Expression& expr, std::size_t depth) {
                    if (depth > 64)
                        throw std::invalid_argument(
                            "source ownership expression exceeds nesting bound");
                    switch (expr.kind) {
                        case Expression::Kind::Integer:
                            return;
                        case Expression::Kind::Variable:
                            if (expr.name != parameter)
                                throw std::invalid_argument(
                                    "state callback reads value outside lexical source authority");
                            return;
                        case Expression::Kind::Field:
                            if (!expr.left ||
                                    expr.left->kind != Expression::Kind::Variable ||
                                    expr.left->name != "state" ||
                                    !state_fields.count(expr.name))
                                throw std::invalid_argument(
                                    "state callback field is outside canonical actor state");
                            return;
                        case Expression::Kind::Negate:
                            if (!expr.left)
                                throw std::invalid_argument(
                                    "malformed unary state expression");
                            visit(*expr.left, depth + 1);
                            return;
                        case Expression::Kind::Add:
                        case Expression::Kind::Subtract:
                        case Expression::Kind::Multiply:
                            if (!expr.left || !expr.right)
                                throw std::invalid_argument(
                                    "malformed binary state expression");
                            visit(*expr.left, depth + 1);
                            visit(*expr.right, depth + 1);
                            return;
                        case Expression::Kind::Call:
                        case Expression::Kind::ArrayCall:
                        case Expression::Kind::Copy:
                        case Expression::Kind::Take:
                        case Expression::Kind::Share:
                        case Expression::Kind::Borrow:
                        case Expression::Kind::BorrowMut:
                            throw std::invalid_argument(
                                "state callback cannot call, transfer, or borrow");
                    }
                    throw std::invalid_argument(
                        "unsupported state callback expression");
                };
                visit(root, 1);
            };

        if (access == backend::ManagedRefAccess::Read && !loan.writes.empty())
            throw std::invalid_argument(
                "read state loan cannot carry source writes");
        for (const auto& write : loan.writes) {
            if (access != backend::ManagedRefAccess::Write ||
                    !state_fields.count(write.field) || !write.expression)
                throw std::invalid_argument(
                    "state write is outside verified source state");
            validate_callback_expression(*write.expression);
        }
        if (!loan.result)
            throw std::invalid_argument(
                "state loan must copy a scalar result out of the callback");
        validate_callback_expression(*loan.result);

        const auto validate_outer_result =
            [&](const Expression& root) {
                std::function<void(const Expression&, std::size_t)> visit;
                visit = [&](const Expression& expr, std::size_t depth) {
                    if (depth > 64)
                        throw std::invalid_argument(
                            "source ownership outer expression exceeds nesting bound");
                    switch (expr.kind) {
                        case Expression::Kind::Integer:
                            return;
                        case Expression::Kind::Variable:
                            if (expr.name != loan.result_binding &&
                                    expr.name != parameter)
                                throw std::invalid_argument(
                                    "state loan result escapes through unknown value");
                            return;
                        case Expression::Kind::Negate:
                            if (!expr.left)
                                throw std::invalid_argument(
                                    "malformed outer unary expression");
                            visit(*expr.left, depth + 1);
                            return;
                        case Expression::Kind::Add:
                        case Expression::Kind::Subtract:
                        case Expression::Kind::Multiply:
                            if (!expr.left || !expr.right)
                                throw std::invalid_argument(
                                    "malformed outer binary expression");
                            visit(*expr.left, depth + 1);
                            visit(*expr.right, depth + 1);
                            return;
                        default:
                            throw std::invalid_argument(
                                "state-backed authority may only leave callback as copied scalar");
                    }
                };
                visit(root, 1);
            };
        if (!method.result)
            throw std::invalid_argument(
                "state actor handler requires copied scalar result");
        validate_outer_result(*method.result);

        if (loan.offset == std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument(
                "state loan source offset cannot identify ownership provenance");
        const std::uint64_t loan_id =
            static_cast<std::uint64_t>(loan.offset) + UINT64_C(1);
        if (loan_id == 0)
            throw std::invalid_argument(
                "state loan source provenance overflow");

        ownership::Graph graph;
        graph.entry.emplace(
            "state.root", ownership::PlaceState{ownership::kOwned});
        ownership::BasicBlock acquire;
        acquire.operations.emplace_back(
            access == backend::ManagedRefAccess::Write
                ? ownership::Operation::Kind::BorrowExclusive
                : ownership::Operation::Kind::BorrowShared,
            "state.root", loan_id);
        acquire.successors.push_back(1);

        ownership::BasicBlock release;
        release.operations.emplace_back(
            access == backend::ManagedRefAccess::Write
                ? ownership::Operation::Kind::ReleaseExclusive
                : ownership::Operation::Kind::ReleaseShared,
            "state.root", loan_id);
        graph.blocks.push_back(std::move(acquire));
        graph.blocks.push_back(std::move(release));

        CriticalStateCallback callback{};
        callback.intrinsic_critical = true;
        callback.callback_critical = loan.critical;
        callback.lexical = true;
        callback.one_state_parameter = true;
        callback.escapes = false;
        callback.state_loan_live_across_suspend = false;
        callback.continuation_generation_fenced = false;
        callback.inferred.bits =
            static_cast<std::uint32_t>(required_effect);

        return backend::ActorRefOwnershipVerifier::
            derive_verified_state_loan_claims(
                graph, 1, "state.root", callback,
                method.declared_effects, access,
                actor_region_capacity_bytes, native64_target);
    }
};

} // namespace oreslang
