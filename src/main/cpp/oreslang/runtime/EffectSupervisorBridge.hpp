#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"
#include "oreslang/runtime/SupervisorFailureLane.hpp"

#include <stdexcept>
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
public:
    static bool publish_terminal(
            SupervisorFailureLane& lane,
            const SupervisorFailureLane::ActorKey& actor,
            EffectStatus effect,
            std::string_view diagnostic,
            std::string_view parent_id = {}) {
        using Effect = EffectKind;
        using Failure = SupervisorFailureLane::Kind;
        Failure classification;
        switch (effect.kind) {
            case Effect::Return: return false;
            case Effect::Throw: classification = Failure::Throw; break;
            case Effect::Raise: classification = Failure::Raise; break;
            case Effect::Panic: classification = Failure::Panic; break;
            case Effect::Cancel: classification = Failure::Cancel; break;
            case Effect::Fatal: classification = Failure::HostFailure; break;
            default: throw std::invalid_argument("unrecognized native exception effect");
        }
        return lane.publish_preallocated(actor, classification, diagnostic, parent_id);
    }
};

} // namespace oreslang
