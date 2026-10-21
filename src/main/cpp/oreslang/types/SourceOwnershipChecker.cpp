#include "oreslang/types/SourceOwnershipChecker.hpp"

#include "oreslang/backend/ActorRefOwnershipVerifier.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/ActorArena.h"
#include "oreslang/types/FunctionEffects.hpp"
#include "oreslang/types/OwnershipCfg.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang {

SourceManagedRefAuthorization
SourceOwnershipChecker::authorize_actor_managed_ref(
        std::string_view source,
        std::string_view actor_name,
        std::string_view ref_field,
        const TargetAbi& target) {
    // Source text is the trust root for this milestone. Do not accept a
    // caller-constructed Program, CFG or ActorManagedRefMir at this boundary.
    Program program = parse_program(source);

    const auto actor_it = std::find_if(
        program.actors.begin(), program.actors.end(),
        [&](const ActorDecl& actor) { return std::string_view(actor.name) == actor_name; });
    if (actor_it == program.actors.end())
        throw std::invalid_argument("managed Ref evidence requires named source actor");

    const ActorDecl& actor = *actor_it;
    if (!actor.sealed_state)
        throw std::invalid_argument("managed Ref evidence requires sealed actor state");

    const auto ref_it = std::find_if(
        actor.managed_ref_fields.begin(), actor.managed_ref_fields.end(),
        [&](const ActorManagedRefField& field) { return std::string_view(field.name) == ref_field; });
    if (ref_it == actor.managed_ref_fields.end())
        throw std::invalid_argument("managed Ref evidence requires declared Ref<int> state field");

    const Function& method = actor.on_message;
    if (!method.has_declared_effects || !method.actor_state_loan ||
            method.result || !method.actor_field_writes.empty())
        throw std::invalid_argument("managed Ref evidence requires checked source state loan");

    const ActorStateLoan& loan = *method.actor_state_loan;
    if (!loan.managed_ref_result || loan.result ||
            std::string_view(loan.managed_ref_result->field) != ref_field)
        throw std::invalid_argument(
            "managed Ref evidence requires exact source managed-ref use");

    const ActorManagedRefUse& use = *loan.managed_ref_result;
    backend::ManagedRefAccess access;
    std::uint32_t inferred_bits =
        static_cast<std::uint32_t>(FunctionEffect::StateRead);

    if (use.access == ActorManagedRefUse::Access::Read) {
        access = backend::ManagedRefAccess::Read;
        if (use.value)
            throw std::invalid_argument("managed Ref read carries invalid write value");
    } else {
        access = backend::ManagedRefAccess::Write;
        inferred_bits |= static_cast<std::uint32_t>(FunctionEffect::StateWrite);
        if (loan.mode != ActorStateLoan::Mode::Write || !use.value)
            throw std::invalid_argument(
                "managed Ref write requires exclusive source state loan");
    }

    FunctionEffects inferred{inferred_bits};
    if (!method.declared_effects.permits(inferred))
        throw std::invalid_argument(
            "managed Ref source effects exceed declared with bound");

    // Reconstruct the exact ownership fact from the source-level state loan.
    // The graph is compiler-owned: no public caller can submit a fake live
    // loan point or provenance flag.
    const std::string place = "state." + std::string(ref_field);
    ownership::Graph graph;
    graph.entry.emplace(place, ownership::PlaceState{ownership::kOwned});

    constexpr std::uint64_t kSourceLoanSite = UINT64_C(1);
    ownership::BasicBlock acquire;
    acquire.operations.emplace_back(
        access == backend::ManagedRefAccess::Write
            ? ownership::Operation::Kind::BorrowExclusive
            : ownership::Operation::Kind::BorrowShared,
        place, kSourceLoanSite);
    acquire.successors.push_back(1);

    ownership::BasicBlock release;
    release.operations.emplace_back(
        access == backend::ManagedRefAccess::Write
            ? ownership::Operation::Kind::ReleaseExclusive
            : ownership::Operation::Kind::ReleaseShared,
        place, kSourceLoanSite);

    graph.blocks.push_back(std::move(acquire));
    graph.blocks.push_back(std::move(release));

    CriticalStateCallback callback{};
    // The parser proved this is the sealed lexical handle_state/using state
    // callback. callback_critical denotes compiler-authenticated callback
    // admission here; it is not a guest-controlled write privilege.
    callback.callback_critical = true;
    callback.one_state_parameter = true;
    callback.inferred = inferred;

    // ActorArena.c currently enforces a fixed 64 MiB production slab
    // budget. Do not accept a caller-authored capacity claim at this authority
    // boundary; layout facts come from compiler/runtime policy and TargetAbi.
    constexpr std::uint64_t kActorRegionCapacity =
        static_cast<std::uint64_t>(ORES_ACTOR_ARENA_MAX_BYTES);
    const bool native64_target = target.pointer_bits() == 64;

    backend::ActorManagedRefMir claims =
        backend::ActorRefOwnershipVerifier::derive_verified_state_loan_claims(
            graph, 1, place, callback, method.declared_effects, access,
            kActorRegionCapacity, native64_target);

    // This constructor is private and SourceOwnershipChecker is its sole
    // production friend. Validate again after authority is wrapped so future
    // MIR invariants cannot be bypassed accidentally.
    backend::ActorRefOwnershipEvidence evidence(claims);
    (void)backend::verify_actor_compressed32(evidence);
    return SourceManagedRefAuthorization(
        std::move(evidence), std::string(source), std::string(target.triple()),
        actor.name, ref_it->name, use.offset);
}

} // namespace oreslang
