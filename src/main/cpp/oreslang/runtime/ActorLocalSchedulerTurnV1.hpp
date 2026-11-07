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
        ores_native_actor_scheduler_is_current_actor(scheduler, actor) &&
        // A physical owner may continue cleanup in STOPPING. Ownership
        // alone is therefore NOT authorization to read/write ActorLocal.
        // poll_ready() samples the exact live actor incarnation under the
        // scheduler lifecycle mutex and rejects STOP, retire and stale refs.
        ores_native_actor_scheduler_poll_ready(scheduler, actor);
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
    // A host value copy runs outside the scope mutex and may suspend its
    // physical carrier long enough for STOP or actor retirement to win.
    // Recheck the *scheduler* after the copy, not just the scope's open flag.
    // A final check is a bounded linearization point, not a lease against
    // cancellation that follows the check. Guest lowering remains denied.
    auto snapshot = scope.get_copy(stamp, key);
    if (!snapshot || !actor_local_current_turn_v1(scheduler, actor, stamp))
        return std::nullopt;
    return snapshot;
}

template <class T>
bool actor_local_erase_on_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    ActorLocalScopeV1& scope,
    ExecutionStampV1 stamp,
    const ActorLocalKeyV1<T>& key) {
    if (!actor_local_current_turn_v1(scheduler, actor, stamp))
        return false;
    return scope.erase(stamp, key);
}

/*
 * Trusted CONTROL-plane retirement. Call only after the source actor has
 * stopped accepting messages. The generation-specific scheduler completion
 * fence must finish before the logical actor's cells can be reclaimed.
 *
 * Never call this from a carrier of ANY scheduler: wait_done could
 * block a scheduler worker and form a cross-domain deadlock cycle. A failed wait leaves the scope OPEN so the caller
 * can retry after resolving outstanding work; it does not forge quiescence.
 */
inline bool actor_local_close_after_terminal_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    ActorLocalScopeV1& scope,
    ExecutionStampV1 owner) {
    if (!scheduler || !owner.valid_actor() ||
        owner.actor_domain != actor.scheduler_domain ||
        owner.actor_slot != actor.actor_slot ||
        owner.actor_generation != actor.generation ||
        !scope.authorized(owner) ||
        // A carrier belonging to ANY scheduler must not wait on another
        // scheduler's terminal barrier. Cross-domain waits can exhaust
        // every carrier and deadlock the VM, not just the target scheduler.
        ores_native_actor_scheduler_is_any_current_carrier_v1())
        return false;
    if (!ores_native_actor_scheduler_wait_done(scheduler, actor))
        return false;
    return scope.close();
}

} // namespace oreslang::runtime
