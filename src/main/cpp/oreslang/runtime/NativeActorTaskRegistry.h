#pragma once
#include "oreslang/runtime/NativeActorTaskBridge.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-capacity, actor-incarnation-owned Task ledger. The host/compiler
 * preallocates 'entries' and retains every Task object until deregistration.
 * No allocations, new pthreads, or CONTROL callbacks are introduced here.
 * Every source actor Task MUST start through this registry. Untracked direct
 * NativeActorTaskBridge starts remain a trusted-host staging API only.
 */
typedef struct {
    OresNativeActorTask* task;
    OresNativeActorTask* parent;
} OresNativeActorTaskEntry;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeActorTaskEntry* entries;
    size_t capacity, count;
    int initialized, sealed;
} OresNativeActorTaskRegistry;

/* Entries must be caller-owned storage with a lifetime exceeding the registry.
 * Capacity is bounded and nonzero, never implicitly extended. */
int ores_native_actor_task_registry_init(
    OresNativeActorTaskRegistry* registry,
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    OresNativeActorTaskEntry* entries, size_t capacity);

/* Registration and task.start are atomic against registry_stop. An optional
 * parent must be a tracked task currently RUNNING on this exact actor carrier:
 * no caller-supplied pointer alone can mint nested-task ownership.
 * Returns 1 only when scheduler-issued initial WAKE was admitted. */
int ores_native_actor_task_registry_start(
    OresNativeActorTaskRegistry* registry, OresNativeActorTask* task,
    OresNativeActorTask* parent);

/* Linearization gate for actor STOP: seals registry before canceling ALL
 * tracked tasks and stopping the exact incarnation; no task may start after
 * that seal. This is nonparking and does not free any Task record. The host
 * must use registry_stop (not raw scheduler_stop) for source Task actors. */
int ores_native_actor_task_registry_stop(
    OresNativeActorTaskRegistry* registry);

/* Physical TaskHandle.get_done proxy. Parent DONE requires its own after-WAKE
 * retirement plus every tracked descendant's physical retirement; successful
 * completion of the parent's scalar result alone is insufficient. */
int ores_native_actor_task_registry_scope_done(
    OresNativeActorTaskRegistry* registry, OresNativeActorTask* task);

/* A child must be deregistered before its parent, even after DONE; this
 * preserves all parent-chain pointers during lifetime accounting. External
 * Future publisher callbacks must be separately revoked/joined by the host
 * before freeing the Task object, even after physical DONE. */
int ores_native_actor_task_registry_unregister(
    OresNativeActorTaskRegistry* registry, OresNativeActorTask* task);
size_t ores_native_actor_task_registry_count(
    OresNativeActorTaskRegistry* registry);
int ores_native_actor_task_registry_close(
    OresNativeActorTaskRegistry* registry);

#ifdef __cplusplus
}
#endif
