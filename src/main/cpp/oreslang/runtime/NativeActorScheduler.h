#pragma once
#include <stddef.h>
#include <stdint.h>
#include "oreslang/runtime/NativeExecutionContext.h"
typedef struct OresNativePhysicalTurnGate OresNativePhysicalTurnGate;
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorScheduler OresNativeActorScheduler;
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresNativeActorRef;
enum { ORES_NATIVE_CONTROL_PANIC = 1, ORES_NATIVE_CONTROL_CANCELLED = 2,
       ORES_NATIVE_CONTROL_STOPPED = 3 };
typedef struct {
    uint64_t kind;
    OresNativeActorRef child;
    uint64_t failure_code;
    uint64_t correlation;
} OresNativeActorTerminalFailure;
/* Typed terminal failure is copied into its child's pre-reserved receipt.
 * The associated supervisor's single logical lane dispatches and acknowledges
 * each child receipt after callback return. No actor-memory pointer is shared.
 */
typedef void (*OresNativeActorPanicFn)(
    void* ctx, size_t parent_actor, OresNativeActorTerminalFailure failure);
/* Explicit *native host* actor lifecycle, not compiler-lowered source spawn.
 * Spawning activates exactly one preallocated actor slot; no ambient cross-
 * actor mutable references, host heap allocation, or slot reincarnation.
 */
/* Trusted host-only pre-admission wiring. Must be called before the first
 * actor message/carrier starts. Gate outlives this scheduler and is released
 * after its last callback finishes. A NULL gate leaves legacy dispatch intact. */
int ores_native_actor_scheduler_configure_turn_gate(
    OresNativeActorScheduler* scheduler, OresNativePhysicalTurnGate* gate);
/* Wake idle carriers that lost a cross-domain permit race, including while
 * the scheduler is retiring and still has admitted work to drain. Trusted
 * VM-only call; not a guest CONTROL message. */
void ores_native_actor_scheduler_signal_turn_gate(OresNativeActorScheduler* scheduler);
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
/* Send succeeds only after atomic admission to a live, generation-matched
 * actor mailbox. It does NOT promise execution or successful processing.
 * No guest callback, blocking wait, or memory allocation occurs here.
 * Cross-process admission cannot be inferred from an IPC write: callers must
 * obtain a remote mailbox receipt before reporting ACCEPTED.
 */
typedef enum {
    ORES_NATIVE_SEND_ACCEPTED = 0,
    ORES_NATIVE_SEND_INVALID_REF = 1,
    ORES_NATIVE_SEND_TERMINATED = 2,
    ORES_NATIVE_SEND_MAILBOX_FULL = 3,
    ORES_NATIVE_SEND_RESOURCE_EXHAUSTED = 4,
    ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE = 5
} OresNativeSendStatus;
OresNativeSendStatus ores_native_actor_scheduler_try_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
/* Compatibility API: 1 iff accepted, 0 otherwise. Prefer typed try_send. */
int ores_native_actor_scheduler_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
/* Logical STOP is refused while any directly bound child incarnation has
 * no accepted terminal failure receipt, even if that child is already
 * physically stopped. Accepted receipts continue to drain on the stopped
 * supervisor's exclusive lane. Global scheduler retire() is a distinct
 * fail-closed teardown and requires external host reconciliation.
 */
int ores_native_actor_scheduler_stop(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Nonblocking host readiness/termination observations. Source-level
 * get_ready()/get_done() must return futures when compiler lowering exists.
 */
int ores_native_actor_scheduler_poll_ready(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_poll_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Trusted external supervisor barrier: block only the host caller until
 * the selected stopped actor drains. This is NOT a guest await/Future;
 * never invoke it on a scheduler worker/actor carrier. */
int ores_native_actor_scheduler_wait_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Handler runs on the scheduler worker pool, never on the submitting carrier.
 * At most one callback executes per actor, across all worker threads.
 * The actor index is a scheduler-local slot, not a cross-tenant capability.
 */
typedef void (*OresNativeActorTaskFn)(void* ctx, size_t actor_index, uint64_t payload);
/* One reserved CONTROL message per actor, independent of normal-mail
 * capacity; only an admitted actor lane may invoke the CONTROL handler.
 * CONTROL callbacks never run on a failed child's carrier by direct call.
 */
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_future,
    OresNativeActorPanicFn on_panic, void* ctx);
OresNativeActorScheduler* ores_native_actor_scheduler_open_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_control, void* ctx);
/* Ordinary single-slot CONTROL is still capacity limited; STOP/CANCEL
 * admission fences must be out of band and not rely on this guest-inaccessible
 * optional control payload queue. This entrypoint does not publish failures.
 */
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent, uint64_t payload);
/* Trusted bootstrap-only supervisor-child association. Must be established
 * BEFORE the child's first accepted DATA/ordinary CONTROL operation; late
 * binding after admission is rejected, even if that work has since drained.
 * Never callable from guest code. Immutable for this scheduler incarnation.
 */
int ores_native_actor_scheduler_bind_supervisor(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent,
    OresNativeActorRef child);
/* One typed terminal receipt per pre-bound, generation-checked child.
 * Only a STOPPED, physically retired child with no active/queued callbacks
 * and a TERMINATED execution authority may report its terminal event. Logical
 * stop() is NOT a finalization receipt. The VM additionally requires a trusted
 * confirmation of host-owned cleanup before making this call.
 * Durable admission is independent of the ordinary CONTROL slot and USER
 * mailbox fullness; distinct child events cannot overwrite each other.
 * Return 0 for duplicate, premature, unbound, stale, stopped parent,
 * or resource exhaustion.
 * A successful receipt is acknowledged only after the supervisor callback
 * returns and the physical actor gate is released; it is NOT a promise of
 * application-level recovery or a crash-persistent transport guarantee.
 * Never invokes on_panic on the caller's carrier.
 */
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure);
uint64_t ores_native_actor_scheduler_control_completed(
    OresNativeActorScheduler* scheduler);
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
/* Whether this OS thread is currently executing a callback from scheduler.
 * Only intended for immediate deadlock prevention in VM teardown. */
int ores_native_actor_scheduler_is_current_carrier(OresNativeActorScheduler* scheduler);
/* Exact live actor identity for trusted callback/FFI bridges only. */
int ores_native_actor_scheduler_is_current_actor(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Logical execution identity for the current guest turn. The execution id is
 * not a heap id and is not a carrier/thread id. For actor turns the allocation
 * domain names the actor-owned domain independently. Returns zero outside an
 * authenticated current turn. */
int ores_native_actor_scheduler_current_execution_context(
    OresNativeActorScheduler* scheduler, OresNativeExecutionContext* out);
/* Host CONTROL-plane wake only: signals an existing scheduler condition.
 * Does not admit actor mail, start workers, invoke guest code, or count as
 * a supervisor delivery receipt. Returns zero once retired.
 * Caller must fence the scheduler's lifetime. */
int ores_native_actor_scheduler_signal_control(OresNativeActorScheduler* scheduler);
/* Requires retirement, drained tasks AND caller-owned submitter fence.
 * Returns 0 when not ready; after success the pointer is invalid.
 */
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* scheduler);
/* Carriers created on demand, initially zero; includes workers until retirement. */
size_t ores_native_actor_scheduler_started_carriers(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* scheduler);
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
/* Test-only deterministic worker-create failure; no production symbol. */
void ores_native_actor_scheduler_test_fail_next_create(void);
#endif
#ifdef ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION
/* Test-only execution-id exhaustion control; no production symbol. */
int ores_native_actor_scheduler_test_set_next_execution(
    OresNativeActorScheduler* scheduler, uint64_t next_execution);
#endif
#ifdef __cplusplus
}
#endif
