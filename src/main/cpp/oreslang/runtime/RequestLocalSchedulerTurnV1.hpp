#pragma once

// Trusted-host RequestLocal adapter. Guest .atto must not invoke raw
// RequestContextLocalV1 methods: possession of a pointer is not proof of
// currently executing an admitted HTTP request or a source actor turn.
//
// This bridge authenticates the ACTOR scheduler claim and actor allocation
// domain. The request itself is supplied by trusted HTTP ingress; this v1
// bridge now enforces an exact scheduler-minted DATA origin but does not
// authenticate tenant/isolate/request membership from a compiler-owned
// request continuation. Guest exposure remains denied.

#include "oreslang/runtime/RequestContextLocalV1.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <optional>
#include <utility>

namespace oreslang::runtime {

inline bool request_local_current_actor_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    const RequestContextLocalV1& request) {
    if (!scheduler || !request.belongs_to(actor) ||
        !ores_native_actor_scheduler_is_current_actor(scheduler, actor))
        return false;
    OresNativeExecutionContext context{};
    if (!ores_native_actor_scheduler_current_execution_context(
            scheduler, &context))
        return false;
    // A current actor turn is not a request-origin proof. A pooled actor
    // can process another HTTP request or a later unrelated DATA message.
    // The scheduler returns 0 for WAKE, CONTROL, STOP and foreign carriers.
    std::uint64_t admitted_mail = 0;
    if (!ores_native_actor_scheduler_current_data_mail_sequence(
            scheduler, actor, &admitted_mail)) return false;
    return context.id.runtime_domain == actor.scheduler_domain &&
        context.id.execution != 0 &&
        context.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR &&
        context.allocation.domain == actor.scheduler_domain &&
        context.allocation.owner == actor.actor_slot &&
        context.allocation.generation == actor.generation &&
        request.identity().valid() &&
        request.bound_to_original_mail(admitted_mail);
}

template <typename T>
bool request_local_set_on_actor_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    RequestContextLocalV1& request,
    const RequestLocalKeyV1<T>& key,
    T value) {
    if (!request_local_current_actor_turn_v1(scheduler, actor, request))
        return false;
    return request.set(key, std::move(value), scheduler);
}

template <typename T>
std::optional<T> request_local_get_on_actor_turn_v1(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    const RequestContextLocalV1& request,
    const RequestLocalKeyV1<T>& key) {
    if (!request_local_current_actor_turn_v1(scheduler, actor, request))
        return std::nullopt;
    auto snapshot = request.get_copy(key);
    // Copying a host value may run user code, block or race with cancellation
    // and STOP. Reauthenticate the physical turn before releasing its copy.
    if (!snapshot || !request_local_current_actor_turn_v1(
                         scheduler, actor, request)) return std::nullopt;
    return snapshot;
}

} // namespace oreslang::runtime
