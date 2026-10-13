#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"
#include "oreslang/runtime/SupervisorFailureLane.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <stdexcept>
#include <string_view>

namespace oreslang {

/**
 * Host-runtime bridge from typed native unwind outcomes to the reserved
 * supervisor CONTROL lane. Actor-terminal Throw/Raise are promoted to
 * supervisor-visible Panic; original Throw/Raise is retained as typed origin.
 * This is intentionally separate from the data
 * mailbox; it cannot execute supervisor callbacks on the failing actor.
 *
 * The caller must reserve a child slot before its execution begins and retire
 * that reservation after successful completion. Only one terminal effect can
 * be published per generation. This is NOT source actor lowering yet.
 */
class EffectSupervisorBridge final {
public:
    /**
     * Native carrier integration: deliver a typed completed unwind directly
     * to the parent's pre-reserved supervisor CONTROL lane. The caller then
     * invokes the parent's lifetime-gated wake handle. A child carrier must
     * never invoke admit_turn() or guest supervisor callbacks itself.
     */
    static bool publish_terminal(
            SupervisorControlTurn& supervisor,
            const SupervisorFailureLane::ActorKey& actor,
            EffectStatus effect,
            std::string_view diagnostic,
            std::string_view parent_id = {}) {
        using Effect = EffectKind;
        using Failure = SupervisorFailureLane::Kind;
        if (effect.kind == Effect::Return) return false;
        const Failure origin = original_kind(effect.kind);
        const Failure delivered = terminal_kind(effect.kind);
        return supervisor.publish_child(
            actor, delivered, diagnostic, parent_id, origin, effect.code);
    }

    static bool publish_terminal(
            SupervisorFailureLane& lane,
            const SupervisorFailureLane::ActorKey& actor,
            EffectStatus effect,
            std::string_view diagnostic,
            std::string_view parent_id = {}) {
        using Effect = EffectKind;
        using Failure = SupervisorFailureLane::Kind;
        if (effect.kind == Effect::Return) return false;
        const Failure origin = original_kind(effect.kind);
        const Failure delivered = terminal_kind(effect.kind);
        return lane.publish_preallocated(actor, delivered, diagnostic, parent_id, origin, effect.code);
    }

private:
    static SupervisorFailureLane::Kind original_kind(EffectKind kind) {
        using K = SupervisorFailureLane::Kind;
        switch (kind) {
            case EffectKind::Throw: return K::Throw;
            case EffectKind::Raise: return K::Raise;
            case EffectKind::Panic: return K::Panic;
            case EffectKind::Cancel: return K::Cancel;
            case EffectKind::Fatal: return K::HostFailure;
            default: throw std::invalid_argument("unknown actor terminal effect");
        }
    }

    static SupervisorFailureLane::Kind terminal_kind(EffectKind kind) {
        // This promotion is actor-boundary-specific. Ordinary non-actor
        // call frames continue propagating until their task/root boundary.
        using K = SupervisorFailureLane::Kind;
        switch (kind) {
            case EffectKind::Throw:
            case EffectKind::Raise:
            case EffectKind::Panic: return K::Panic;
            case EffectKind::Cancel: return K::Cancel;
            case EffectKind::Fatal: return K::HostFailure;
            default: throw std::invalid_argument("unknown actor terminal effect");
        }
    }
};

} // namespace oreslang
