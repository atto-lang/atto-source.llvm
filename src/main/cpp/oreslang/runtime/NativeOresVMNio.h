#pragma once
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeEpollReactorABI.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Trusted host-only VM NIO integration. All of these functions require the
 * caller to retain a live VM pointer and externally fence producer threads
 * before VM retirement/close. They never run guest code.
 *
 * NIO configuration is an explicit bootstrap operation. Throughput mode
 * attaches a zero-thread reactor domain and starts reactor #0 only on the
 * first valid readiness watch. Preallocated RT mode starts its fixed configured
 * reactor set before source guest admission. No hard-RT WCET is claimed.
 *
 * These bare watch/cancel/pop calls are outer-host only and fail closed when
 * invoked from a VM actor carrier. Source-actor NIO must first bind the exact
 * ActorRef to a physical resource loan and a generation-fenced continuation;
 * using this host ABI directly must never make get_done() observable early.
 */
int ores_native_vm_enable_nio(OresNativeVM* vm, size_t max_reactors,
                              int preallocated_realtime, size_t rt_reactors);
size_t ores_native_vm_nio_started(OresNativeVM* vm);
int ores_native_vm_nio_try_watch(OresNativeVM* vm, int fd, uint32_t events,
                                 OresNativeNioWatch* output);
typedef struct {
    OresNativeNioWatch watch;
    enum OresNativeVMDomain domain;
    OresNativeActorRef actor;
    uint32_t claim_slot;       /* 1-based bounded manager claim slot */
    uint64_t claim_generation; /* never zero; stale tokens fail closed */
} OresNativeActorNioWatch;

typedef struct {
    OresNativeNioCompletion io;
    OresNativeActorNioWatch owner;
} OresNativeActorNioCompletion;

typedef struct {
    OresNativeActorNioWatch watch;
    OresNativeWakeTicket wake;
} OresNativeActorNioAwait;

/* Result of handing one delivered NIO completion into scheduler wake escrow. */
typedef enum {
    ORES_NATIVE_NIO_AWAIT_INVALID = 0,
    ORES_NATIVE_NIO_AWAIT_PUBLISHED = 1,
    ORES_NATIVE_NIO_AWAIT_PRODUCER_RETIRED = 2
} OresNativeActorNioAwaitDispatchStatus;

/*
 * Actor-owned readiness bridge. Admission first retains one exact ActorRef
 * physical-resource loan, then registers the kernel watch under a bounded
 * generation-checked claim. If STOP/retire wins first, admission fails before
 * epoll ownership. A terminal completion keeps the resource loan and FD claim
 * alive until ack_actor_completion() after continuation/wake escrow transfer.
 *
 * Actor-carrier calls must name the exact currently executing ActorRef.
 * Trusted outer-host runtime code may also use these functions on behalf of
 * that exact ActorRef. Bare host watch/cancel/pop APIs remain disjoint.
 */
int ores_native_vm_nio_try_watch_actor(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    int fd, uint32_t events, OresNativeActorNioWatch* output);
/* Reserve scheduler wake escrow first, then admit the actor-owned kernel
 * watch under the same exact ActorRef. This creates a continuous physical-DONE
 * fence: STOP cannot complete between suspension admission and later scheduler
 * wake publication. target is currently the same actor; cross-actor Future
 * routing remains a separate capability boundary. */
int ores_native_vm_nio_try_watch_actor_await(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    int fd, uint32_t events,
    uint64_t continuation_slot, uint64_t continuation_generation,
    uint64_t code_generation, uint64_t descriptor_generation,
    OresNativeActorNioAwait* output);
int ores_native_vm_nio_try_cancel_actor(
    OresNativeVM* vm, OresNativeActorNioWatch watch);
int ores_native_vm_nio_try_pop_actor(
    OresNativeVM* vm, OresNativeActorNioCompletion* output);
int ores_native_vm_nio_ack_actor_completion(
    OresNativeVM* vm, OresNativeActorNioCompletion completion);
/* For await-bound claims, plain ACK is rejected. This operation transfers
 * the reserved wake, publishes READY/CANCELLED/FAILED onto the actor lane, and
 * only then releases the NIO resource loan/FD claim. The reactor/completion
 * thread never invokes guest code. */
OresNativeActorNioAwaitDispatchStatus
ores_native_vm_nio_dispatch_actor_await(
    OresNativeVM* vm, OresNativeActorNioCompletion completion);

int ores_native_vm_nio_try_cancel(OresNativeVM* vm, OresNativeNioWatch watch);
int ores_native_vm_nio_try_pop(OresNativeVM* vm, OresNativeNioCompletion* output);
/* Collect up to min(capacity, 256) HOST-owned completed tickets into
 * caller-owned storage under one VM backend lease. Actor-owned completions are
 * retained in the bounded actor claim table until try_pop_actor() consumes
 * them. Each record is consumed exactly once.
 * Zero means no work, contention, or a retired VM. No allocations, blocking,
 * or guest callbacks occur on this operation path. Host scheduling and Future
 * resolution remain the caller's responsibility.
 */
size_t ores_native_vm_nio_try_pop_batch(
    OresNativeVM* vm, OresNativeNioCompletion* output, size_t capacity);
int ores_native_vm_nio_completion_fd(OresNativeVM* vm, size_t reactor);
void ores_native_vm_nio_drain_signal(OresNativeVM* vm, size_t reactor);

#ifdef __cplusplus
}
#endif
