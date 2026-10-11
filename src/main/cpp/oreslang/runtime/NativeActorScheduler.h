#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorScheduler OresNativeActorScheduler;
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresNativeActorRef;
/* Explicit *native host* actor lifecycle, not compiler-lowered source spawn.
 * Spawning activates exactly one preallocated actor slot; no ambient cross-
 * actor mutable references, host heap allocation, or slot reincarnation.
 */
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
int ores_native_actor_scheduler_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
int ores_native_actor_scheduler_stop(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Nonblocking host readiness/termination observations. Source-level
 * get_ready()/get_done() must return futures when compiler lowering exists.
 */
int ores_native_actor_scheduler_poll_ready(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_poll_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Handler runs on the scheduler worker pool, never on the submitting carrier.
 * At most one callback executes per actor, across all worker threads.
 * The actor index is a scheduler-local slot, not a cross-tenant capability.
 */
typedef void (*OresNativeActorTaskFn)(void* ctx, size_t actor_index, uint64_t payload);
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx);
/* Legacy index admission requires an already-spawned host actor.\n * New hosts should use the generation-checked actor ref send API.\n * O(1), nonblocking admission. Returns 1 on accepted, 0 on capacity or retirement.
 * Does not allocate and never invokes callbacks inline. Capacity includes the
 * currently executing task. Caller retains ownership on rejection.
 */
int ores_native_actor_scheduler_submit(
    OresNativeActorScheduler* scheduler, size_t actor_index, uint64_t payload);
/* No new tasks after retirement, but previously accepted tasks drain. */
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* scheduler);
/* Wait for every accepted callback to complete; may not be called from its worker. */
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* scheduler);
/* Requires retirement, drained tasks AND caller-owned submitter fence.
 * Returns 0 when not ready; after success the pointer is invalid.
 */
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* scheduler);
#ifdef __cplusplus
}
#endif
