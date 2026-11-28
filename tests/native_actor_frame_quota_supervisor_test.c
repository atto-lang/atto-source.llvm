/* Native *scheduler + frame-store* integration: an independent actor B
 * executes while A exhausts a bounded logical frame. The trusted host
 * publishes the terminal failure on priority supervisor CONTROL after STOP.
 * A new physical scheduler slot models a restart with a new logical actor
 * incarnation. This is NOT compiler-generated .atto failure forwarding. */
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) do { if (!(cond)) { \
  fprintf(stderr, "NATIVE_ACTOR_QUOTA_FAILURE line %d: %s\n", __LINE__, #cond); \
  abort(); } } while (0)

enum { ACTOR_A=1, ACTOR_B=2, ACTOR_A_NEW=3, SUPERVISOR=0 };
#define ACTOR_STACK_QUOTA UINT64_C(0xA7700001)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorLogicalFramesV1* a;
    OresNativeActorLogicalFramesV1* b;
    OresNativeActorLogicalFramesV1* a_new;
    OresNativeActorRef supervisor, first, sibling, restarted;
    OresLogicalFrameV1 stale;
    atomic_int a_quota_denied;
    atomic_int b_returned_42;
    atomic_int supervisor_notices;
    atomic_int a_restart_ok;
} State;

static void receive(void* raw, size_t actor, uint64_t payload) {
    State* state = (State*)raw;
    OresLogicalTurnV1 turn = {0};
    OresLogicalFrameV1 frame = {0};
    if (actor == ACTOR_A) {
        CHECK(payload == 7);
        CHECK(ores_logical_frames_v1_begin(state->a, 701, 1, &turn));
        CHECK(ores_logical_frames_v1_push(state->a, turn, &frame));
        /* One frame is permitted; a second frame is a recoverable quota
         * denial, not a process-wide abort or an unchecked C stack overrun. */
        OresLogicalFrameV1 overflow = {0};
        CHECK(!ores_logical_frames_v1_push(state->a, turn, &overflow));
        state->stale = frame;
        CHECK(ores_logical_frames_v1_cancel(state->a, turn));
        atomic_store_explicit(&state->a_quota_denied, 1, memory_order_release);
    } else if (actor == ACTOR_B) {
        CHECK(payload == 42);
        CHECK(ores_logical_frames_v1_begin(state->b, 702, 1, &turn));
        CHECK(ores_logical_frames_v1_push(state->b, turn, &frame));
        int64_t value = 42, observed = 0;
        CHECK(ores_logical_frames_v1_write(state->b, frame, 0, &value, sizeof value));
        CHECK(ores_logical_frames_v1_read(state->b, frame, 0, &observed, sizeof observed));
        CHECK(observed == 42);
        CHECK(ores_logical_frames_v1_pop(state->b, frame));
        CHECK(ores_logical_frames_v1_end(state->b, turn));
        atomic_store_explicit(&state->b_returned_42, 1, memory_order_release);
    } else if (actor == ACTOR_A_NEW) {
        CHECK(payload == 9);
        CHECK(ores_logical_frames_v1_begin(state->a_new, 701, 2, &turn));
        CHECK(!ores_logical_frames_v1_push(state->a_new, state->stale.turn, &frame));
        CHECK(ores_logical_frames_v1_push(state->a_new, turn, &frame));
        int64_t observed = -1;
        CHECK(!ores_logical_frames_v1_read(state->a_new, state->stale, 0,
                                           &observed, sizeof observed));
        CHECK(ores_logical_frames_v1_read(state->a_new, frame, 0,
                                          &observed, sizeof observed));
        CHECK(observed == 0); /* new actor-owned storage has no prior data */
        CHECK(ores_logical_frames_v1_pop(state->a_new, frame));
        CHECK(ores_logical_frames_v1_end(state->a_new, turn));
        atomic_store_explicit(&state->a_restart_ok, 1, memory_order_release);
    } else {
        CHECK(actor == SUPERVISOR);
        CHECK(0); /* supervisor only receives priority typed CONTROL */
    }
}

static void failure_control(void* raw, size_t parent,
                            OresNativeActorTerminalFailure failure) {
    State* state = (State*)raw;
    CHECK(parent == SUPERVISOR);
    CHECK(failure.kind == ORES_NATIVE_CONTROL_PANIC);
    CHECK(failure.child.scheduler_domain == state->first.scheduler_domain);
    CHECK(failure.child.actor_slot == state->first.actor_slot);
    CHECK(failure.child.generation == state->first.generation);
    CHECK(failure.failure_code == ACTOR_STACK_QUOTA);
    CHECK(failure.correlation == 701);
    CHECK(atomic_load_explicit(&state->a_quota_denied, memory_order_acquire));
    atomic_fetch_add_explicit(&state->supervisor_notices, 1, memory_order_release);
}

int main(void) {
    State state = {0};
    atomic_init(&state.a_quota_denied, 0);
    atomic_init(&state.b_returned_42, 0);
    atomic_init(&state.supervisor_notices, 0);
    atomic_init(&state.a_restart_ok, 0);
    state.a = ores_logical_frames_v1_open(701, 1, 1, 1, sizeof(int64_t));
    state.b = ores_logical_frames_v1_open(702, 1, 1, 1, sizeof(int64_t));
    state.a_new = ores_logical_frames_v1_open(701, 2, 1, 1, sizeof(int64_t));
    CHECK(state.a && state.b && state.a_new);

    state.scheduler = ores_native_actor_scheduler_open_typed_control(
        4, 4, 3, receive, NULL, failure_control, &state);
    CHECK(state.scheduler);
    state.supervisor = ores_native_actor_scheduler_spawn(state.scheduler, SUPERVISOR);
    state.first = ores_native_actor_scheduler_spawn(state.scheduler, ACTOR_A);
    state.sibling = ores_native_actor_scheduler_spawn(state.scheduler, ACTOR_B);
    CHECK(state.supervisor.scheduler_domain && state.first.scheduler_domain
          && state.sibling.scheduler_domain);
    /* Main requires an immutable child->supervisor reservation before the
     * first child DATA message (preallocated per-child CONTROL receipt). */
    CHECK(ores_native_actor_scheduler_bind_supervisor(
        state.scheduler, state.supervisor, state.first));

    CHECK(ores_native_actor_scheduler_send(state.scheduler, state.first, 7));
    CHECK(ores_native_actor_scheduler_send(state.scheduler, state.sibling, 42));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(atomic_load(&state.a_quota_denied) == 1);
    CHECK(atomic_load(&state.b_returned_42) == 1);

    /* This host bridge uses terminal CONTROL only after physical STOP.
     * Do not pretend that the source-language supervisor is wired already. */
    CHECK(ores_native_actor_scheduler_stop_sealed(state.scheduler, state.first));
    CHECK(ores_native_actor_scheduler_poll_done(state.scheduler, state.first));
    CHECK(ores_logical_frames_v1_close(state.a));
    state.a = NULL;

    OresNativeActorTerminalFailure failure = {
        .kind = ORES_NATIVE_CONTROL_PANIC,
        .child = state.first,
        .failure_code = ACTOR_STACK_QUOTA,
        .correlation = 701
    };
    CHECK(ores_native_actor_scheduler_publish_failure(
        state.scheduler, state.supervisor, failure));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(atomic_load(&state.supervisor_notices) == 1);

    /* The host then restarts A at a NEW scheduler slot; never recycle the
     * physical slot or actor incarnation while stale tokens may survive. */
    state.restarted = ores_native_actor_scheduler_spawn(state.scheduler, ACTOR_A_NEW);
    CHECK(state.restarted.scheduler_domain);
    CHECK(!ores_native_actor_scheduler_send(state.scheduler, state.first, 1));
    CHECK(ores_native_actor_scheduler_send(state.scheduler, state.restarted, 9));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(atomic_load(&state.a_restart_ok) == 1);
    CHECK(ores_native_actor_scheduler_stop(state.scheduler, state.restarted));
    CHECK(ores_native_actor_scheduler_stop(state.scheduler, state.sibling));
    CHECK(ores_native_actor_scheduler_stop(state.scheduler, state.supervisor));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(ores_native_actor_scheduler_poll_done(state.scheduler, state.restarted));
    CHECK(ores_native_actor_scheduler_poll_done(state.scheduler, state.sibling));
    CHECK(ores_native_actor_scheduler_poll_done(state.scheduler, state.supervisor));
    CHECK(ores_logical_frames_v1_close(state.a_new));
    CHECK(ores_logical_frames_v1_close(state.b));
    ores_native_actor_scheduler_retire(state.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(state.scheduler));
    puts("NATIVE_ACTOR_A_B_QUOTA_SUPERVISOR_PASS A=quota B=42 notices=1 restarted=1");
    return 0;
}
