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
/* Host-only, thread-affine admission guard. The scheduler pins this exact
 * physical incarnation until release. Never export it to guest code. */
typedef struct {
    OresNativeActorRef actor;
    uint64_t nonce;
} OresNativeRestartLease;
enum { ORES_NATIVE_CONTROL_PANIC = 1, ORES_NATIVE_CONTROL_CANCELLED = 2,
       ORES_NATIVE_CONTROL_STOPPED = 3 };
typedef struct {
    uint64_t kind;
    OresNativeActorRef child;
    uint64_t failure_code;
    uint64_t correlation;
} OresNativeActorTerminalFailure;
/* Typed terminal failure is copied into the reserved parent CONTROL slot.
 * A supervisor handler runs only on the parent's single logical actor lane.
 */
typedef void (*OresNativeActorPanicFn)(
    void* ctx, size_t parent_actor, OresNativeActorTerminalFailure failure);
/* Explicit *native host* actor lifecycle, not compiler-lowered source spawn.
 * Normal spawn claims one preallocated physical slot. The strictly limited
 * recycle_unpublished_done operation reclaims stopped never-published
 * candidates. Previously active actors are recyclable only through the
 * opt-in generation-aware callback mode after physical DONE and a sealed
 * NORMAL exit. Attempted PANIC and legacy slot-callback generations remain
 * quarantined. No guest compiler lowering is implied by the host ABI.
 * No ambient cross-actor mutable references or host heap allocations per
 * message are introduced by this scheduler.
 */
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
/* Replacement candidates are physically allocated but cannot receive normal
 * mail, even through legacy index submit, until publication commits. */
OresNativeActorRef ores_native_actor_scheduler_spawn_unpublished(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
/* Generation-safe reclamation of a failed hidden replacement only.
 * Preconditions: exact old ActorRef is retired, physical DONE, unpublished,
 * never admitted user or CONTROL callbacks, without any active admission
 * lease, and global scheduler is alive. The operation advances the same
 * physical slot to generation+1, keeps it hidden and returns the fresh ref.
 * It can NOT recycle an ACTIVE/published incarnation: legacy message/control
 * callback ABI is slot-index based and must be redesigned for that case.
 * Old refs immediately become stale; legacy index submit is disabled for
 * generation > 1, while ref-based send remains generation-checked.
 * External owner must still prove exact DONE and abort the old registry
 * reservation before publishing the next logical incarnation.
 */
OresNativeActorRef ores_native_actor_scheduler_recycle_unpublished_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef finalized_candidate);
/* Opt-in generation-aware callback mode permits reuse of a previously
 * PUBLISHED actor ONLY after physical DONE and successful normal-exit
 * classification. PANIC-attempted/published generations stay permanently
 * fenced until a separate supervisor terminal-receipt protocol exists.
 * A failed/unsealed actor is NOT eligible. The next generation is
 * UNPUBLISHED; no ordinary message is admitted until its new host lease
 * and logical supervisor registration are committed. Legacy callback-mode
 * schedulers cannot call this API. This does not establish guest RESTART. */
OresNativeActorRef ores_native_actor_scheduler_recycle_normal_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef finalized_actor);
/* Serializes RESTART publication with physical stop/global retirement without
 * holding the scheduler's internal lock over registry operations. A successful
 * lease must be released exactly once on its acquiring host thread; stop and
 * retire wait for that release. Nonrecursive: no stop/retire under a lease. */
int ores_native_actor_scheduler_acquire_restart_lease(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    OresNativeRestartLease* out);
/* Pins an ALREADY published initial actor across logical supervisor
 * registration, using the same STOP/retirement exclusion. This lease cannot
 * publish RESTART and must be released on its acquiring host thread. */
int ores_native_actor_scheduler_acquire_registration_lease(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    OresNativeRestartLease* out);
/* Called only AFTER logical identity/routing publication while the lease is
 * held; makes the physical lane externally routable. */
int ores_native_actor_scheduler_publish_restart_lease(
    OresNativeActorScheduler* scheduler, OresNativeRestartLease lease);
int ores_native_actor_scheduler_release_restart_lease(
    OresNativeActorScheduler* scheduler, OresNativeRestartLease lease);
int ores_native_actor_scheduler_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
int ores_native_actor_scheduler_stop(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Nonblocking host readiness/termination observations. Source-level
 * get_ready()/get_done() must return futures when compiler lowering exists.
 */
int ores_native_actor_scheduler_poll_ready(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Host-only, generation-checked observation of physical routing visibility.
 * READY is not enough: unpublished RESTART actors may be alive but must not
 * accept external messages until scheduler lease publication is complete.
 */
int ores_native_actor_scheduler_poll_published(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_poll_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Trusted normal-exit claim. Atomically verify physical finalization and
 * seal this exact actor generation against any later terminal-failure
 * publication. Refuse if a terminal failure was already published, even
 * if its parent CONTROL callback has not yet been delivered. Only the
 * lifecycle owner may classify the exit as normal; not a guest capability.
 * Returns 1 on the first successful seal, 0 otherwise.
 */
int ores_native_actor_scheduler_seal_normal_exit(
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
/* New callback ABI: the scheduler captures this exact physical actor
 * identity under its mutex at dequeue, before releasing the worker lock.
 * Hosts must fence delayed continuations and any retained callback-owned
 * data using that same ActorRef. No slot-only callback is invoked for
 * an identity-mode scheduler. */
typedef void (*OresNativeActorIdentityTaskFn)(
    void* ctx, OresNativeActorRef actor, uint64_t payload);
typedef void (*OresNativeActorIdentityPanicFn)(
    void* ctx, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure);
/* Explicit opt-in: all callbacks (DATA, CONTROL, typed PANIC) use
 * generation-bearing identity, never the legacy slot-only ABI.
 * Future and PANIC callbacks may be NULL if the corresponding channel
 * is unused. No guest access is granted by selecting this mode. */
OresNativeActorScheduler* ores_native_actor_scheduler_open_identity_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorIdentityTaskFn on_message,
    OresNativeActorIdentityTaskFn on_future,
    OresNativeActorIdentityPanicFn on_panic, void* ctx);
/* Host observer only. A slot-only PANIC bridge entrypoint must reject
 * identity-mode callbacks rather than bypass exact parent generation. */
int ores_native_actor_scheduler_uses_identity_callbacks(
    OresNativeActorScheduler* scheduler);
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
/* A failed/retired child publishes a typed control envelope (encoded in
 * payload) to the parent. Returns 0 if the reserved slot is full or if the
 * target is stale/stopped; caller must retry or explicitly escalate.
 */
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent, uint64_t payload);
/* Child must be a valid actor in the same scheduler, distinct from parent.
 * Returns 0 on reserved CONTROL saturation, stale refs or parent retirement.
 * Never invokes on_panic directly on the publishing child's carrier.
 */
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure);
/* Host-owned, scheduler-local terminal evidence. Recorded atomically at the
 * first authenticated PANIC publication ATTEMPT, even if parent CONTROL is
 * full or physically STOPPED. No malloc, guest callback or external effect.
 * Identity binds the original physical parent and failed child generations.
 *
 * state: RECORDED = never host-admitted; CLAIMED = exactly one host
 * reconciliation owner, outcome potentially uncertain; ACKNOWLEDGED = host
 * attested successful enqueue. A CLAIMED receipt MUST NOT be retried after
 * an ambiguous host error without separate authoritative effect evidence.
 * Receipts live until scheduler destruction; no process-crash durability. */
enum {
    ORES_NATIVE_TERMINAL_RECEIPT_NONE = 0,
    ORES_NATIVE_TERMINAL_RECEIPT_RECORDED = 1,
    ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED = 2,
    ORES_NATIVE_TERMINAL_RECEIPT_ACKNOWLEDGED = 3
};
typedef struct {
    OresNativeActorRef original_parent;
    OresNativeActorTerminalFailure failure;
    uint64_t nonce; /* one-use, host-owned claim token */
    uint64_t status;
    uint64_t was_published_to_control;
} OresNativeTerminalReceipt;
/* Exact generation-bound observation. Stale/foreign refs never alias
 * a recycled incarnation; payload is copied under the scheduler mutex. */
int ores_native_actor_scheduler_terminal_receipt(
    OresNativeActorScheduler* scheduler, OresNativeActorRef original_parent,
    OresNativeActorRef failed_child, OresNativeTerminalReceipt* out);
/* Claim before entering any host CONTROL effect, even if the original
 * physical parent has already STOPPED. Requires exact physical child DONE.
 * One successful claimant; no retry for CLAIMED/ACKNOWLEDGED. */
int ores_native_actor_scheduler_claim_terminal_receipt(
    OresNativeActorScheduler* scheduler, OresNativeActorRef original_parent,
    OresNativeActorRef failed_child, OresNativeTerminalReceipt* out);
/* Acknowledge only for the exact nonce returned by a previous claim,
 * AFTER independently confirmed host CONTROL enqueue. No guest authority. */
int ores_native_actor_scheduler_ack_terminal_receipt(
    OresNativeActorScheduler* scheduler, OresNativeTerminalReceipt receipt);
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
/* No new tasks after retirement, but previously accepted tasks drain.
 * This host-only operation waits for active publication leases to release;
 * never call it from inside a lease-owning host transaction. */
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
