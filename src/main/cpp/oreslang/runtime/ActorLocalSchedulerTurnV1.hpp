#pragma once

// Trusted-host bridge from a REAL scheduler-admitted actor turn to ActorLocal.
// This API deliberately does not make ExecutionStampV1 a guest capability.
// The scheduler and scope must be alive through each call; no pointer is
// retained. A guessed or copied logical identity is never sufficient.

#include "oreslang/runtime/LocalContextDomainsV1.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <optional>
#include <utility>

namespace oreslang::runtime {

inline bool actor_local_current_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    ExecutionStampV1 stamp) noexcept {
    if (!scheduler || !stamp.valid_actor() ||
        stamp.actor_domain != actor.scheduler_domain ||
        stamp.actor_slot != actor.actor_slot ||
        stamp.actor_generation != actor.generation)
        return false;
    // Current logical execution is minted only by the scheduler under its
    // authenticated ActorCell claim. No guest-provided integer stamp is enough.
    OresNativeExecutionContext execution{};
    if (!ores_native_actor_scheduler_current_execution_context(
            scheduler, &execution))
        return false;
    // A scheduler turn ID alone cannot certify actor-owned storage: require
    // the independently recorded allocation domain, slot AND generation.
    // In particular, a same-scheduler turn for another actor is not access.
    return execution.id.runtime_domain == actor.scheduler_domain &&
        execution.id.execution != 0 &&
        execution.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR &&
        execution.allocation.domain == actor.scheduler_domain &&
        execution.allocation.owner == actor.actor_slot &&
        execution.allocation.generation == actor.generation &&
        ores_native_actor_scheduler_is_current_actor(scheduler, actor);
}

template <class T>
bool actor_local_set_on_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    ActorLocalScopeV1& scope,
    ExecutionStampV1 stamp,
    const ActorLocalKeyV1<T>& key, T value) {
    if (!actor_local_current_turn_v1(scheduler, actor, stamp)) return false;
    return scope.set(stamp, key, std::move(value));
}

template <class T>
std::optional<T> actor_local_get_on_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    const ActorLocalScopeV1& scope,
    ExecutionStampV1 stamp,
    const ActorLocalKeyV1<T>& key) {
    if (!actor_local_current_turn_v1(scheduler, actor, stamp))
        return std::nullopt;
    return scope.get_copy(stamp, key);
}

} // namespace oreslang::runtime
