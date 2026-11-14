#pragma once

#include "oreslang/types/OwnershipCfg.hpp"
#include "oreslang/types/OwnershipDropPlan.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types::native_box_v1 {

// Typed plan emitted by the source frontend only after Box<T> was resolved
// as a compiler intrinsic. These events are NOT parsed directly from .atto;
// provenance, alias exclusion and lexical scope are frontend responsibilities.
enum class EventKind {
    Initialize, Read, BorrowRead, BorrowWrite, ReleaseRead, ReleaseWrite,
    TakeInto, Copy, Drop, Suspend, ThreadMove, ActorSend
};
struct Event {
    EventKind kind;
    std::string owner;
    std::string destination;
    std::uint64_t loan_id = 0; // zero: derive from unique source event site
    bool verified_send = false; // compiler-derived, never source-asserted
};
struct Plan {
    bool trusted_box_intrinsic = false;
    std::vector<std::string> unique_places;
    std::vector<Event> events;
};
struct Proof {
    ownership::DropExit exit;
    ownership::State terminal;
};

// A deliberately fail-closed straight-line Box<...> source effect checker.
// It reuses the same field-sensitive loan identity machinery that protects
// native class owners. Future control-flow lowering must use the full CFG.
inline Proof check(const Plan& plan) {
    using K = ownership::Operation::Kind;
    if (!plan.trusted_box_intrinsic)
        throw std::runtime_error("Box source intrinsic identity unverified");
    if (plan.unique_places.empty() || plan.unique_places.size() > 64 ||
            plan.events.size() > 4096)
        throw std::runtime_error("Box ownership plan exceeds fixed proof bounds");
    ownership::Graph graph;
    for (const auto& place : plan.unique_places) {
        if (place.empty() || place.find('.') != std::string::npos ||
                !graph.entry.emplace(place, ownership::PlaceState{}).second)
            throw std::runtime_error("duplicate or invalid Box owner place");
    }
    auto& effects = graph.blocks.emplace_back().operations;
    ownership::State state = graph.entry;
    std::uint64_t site = 1;
    const auto apply = [&](K kind, const std::string& owner,
                           std::uint64_t id = 0) {
        ownership::Operation op{kind, owner, id ? std::optional<std::uint64_t>(id)
                                               : std::nullopt};
        ownership::apply(op, state, site++);
        effects.push_back(std::move(op));
    };
    for (const auto& event : plan.events) {
        if (site > 8190) throw std::runtime_error("Box site budget exhausted");
        switch (event.kind) {
        case EventKind::Initialize: apply(K::Initialize, event.owner); break;
        case EventKind::Read: apply(K::Read, event.owner); break;
        case EventKind::BorrowRead:
            apply(K::BorrowShared, event.owner, event.loan_id); break;
        case EventKind::BorrowWrite:
            apply(K::BorrowExclusive, event.owner, event.loan_id); break;
        case EventKind::ReleaseRead:
            apply(K::ReleaseShared, event.owner, event.loan_id); break;
        case EventKind::ReleaseWrite:
            apply(K::ReleaseExclusive, event.owner, event.loan_id); break;
        case EventKind::TakeInto:
            if (event.owner == event.destination ||
                    !graph.entry.count(event.destination))
                throw std::runtime_error("Box transfer requires a separate known destination");
            apply(K::Move, event.owner);
            apply(K::Initialize, event.destination);
            break;
        case EventKind::Copy:
            throw std::runtime_error("Box is affine and cannot be copied");
        case EventKind::Drop: apply(K::Drop, event.owner); break;
        case EventKind::Suspend:
            // Without pinned-frame + borrow checking across suspension, a
            // live Box on any lexical stack cannot safely be suspended.
            for (const auto& [name, place] : state)
                if ((place.possible & ownership::kOwned) ||
                    place.shared_loans || place.exclusive_loan)
                    ownership::violation(name, "Box live across unsupported suspension");
            break;
        case EventKind::ThreadMove:
            if (!event.verified_send)
                throw std::runtime_error("Box<T> thread transfer lacks verified T:Send");
            apply(K::Move, event.owner); // consumes source, never grants Share
            break;
        case EventKind::ActorSend:
            throw std::runtime_error(
                "Box actor send requires relocation/serialization and destination heap authority");
        }
    }
    const auto analysis = ownership::verify(graph);
    const auto drops = ownership::plan_drop_exits(graph, plan.unique_places);
    if (drops.size() != 1 || !analysis.outgoing[0])
        throw std::runtime_error("Box exit has no verified exact-once drop plan");
    return Proof{drops.front(), *analysis.outgoing[0]};
}
} // namespace oreslang::types::native_box_v1
