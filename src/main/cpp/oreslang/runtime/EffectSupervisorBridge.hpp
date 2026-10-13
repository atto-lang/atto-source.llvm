#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"
#include "oreslang/runtime/SupervisorFailureLane.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <string_view>

namespace oreslang {

/**
 * Host-runtime bridge from typed native unwind outcomes to the reserved
 * supervisor CONTROL lane. This is intentionally separate from the data
 * mailbox; it cannot execute supervisor callbacks on the failing actor.
 *
 * The caller must reserve a child slot before its execution begins and retire
 * that reservation after successful completion. Only one terminal effect can
 * be published per generation. This is NOT source actor lowering yet.
 */
class EffectSupervisorBridge final {
private:
    // Normalize each native/FFI effect at the final actor-control boundary,
    // even when callers bypass EffectUnwind. Never throw a C++ exception for
    // an unknown discriminant: a reserved child slot would remain stranded.
    static SupervisorFailureLane::Kind classify_terminal(EffectKind kind) noexcept {
        using Kind = SupervisorFailureLane::Kind;
        switch (kind) {
            case EffectKind::Throw: return Kind::Throw;
            case EffectKind::Raise: return Kind::Raise;
            case EffectKind::Panic: return Kind::Panic;
            case EffectKind::Cancel: return Kind::Cancel;
            case EffectKind::Fatal:
            case EffectKind::ProcessExit:
            case EffectKind::Return: return Kind::HostFailure;
        }
        return Kind::HostFailure; // Fail closed for future/invalid effect tags.
    }

    struct Admission {
        EffectKind kind;
        SupervisorFailureLane::Kind classification;
        std::string_view diagnostic;
    };

    static Admission admit(EffectStatus effect, std::string_view diagnostic) noexcept {
        const bool forged = !effect_kind_valid(effect.kind);
        const EffectStatus canonical = normalize_effect(effect);
        return Admission{canonical.kind, classify_terminal(canonical.kind),
            forged ? std::string_view{"invalid native effect discriminant"} : diagnostic};
    }

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
        const Admission admitted = admit(effect, diagnostic);
        if (admitted.kind == EffectKind::Return) return false;
        return supervisor.publish_child(
            actor, admitted.classification, admitted.diagnostic, parent_id);
    }

    static bool publish_terminal(
            SupervisorFailureLane& lane,
            const SupervisorFailureLane::ActorKey& actor,
            EffectStatus effect,
            std::string_view diagnostic,
            std::string_view parent_id = {}) {
        const Admission admitted = admit(effect, diagnostic);
        if (admitted.kind == EffectKind::Return) return false;
        return lane.publish_preallocated(
            actor, admitted.classification, admitted.diagnostic, parent_id);
    }
};

} // namespace oreslang
