#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorBytesSchedulerBinding bindings[2];
    atomic_int completed;
} Context;
static void check(int ok) { if (!ok) abort(); }

/* Genuine existing native scheduler worker callback, not manual main call. */
static void on_data(void* opaque, size_t actor_index, uint64_t payload) {
    Context* ctx = (Context*)opaque;
    check(actor_index < 2 && payload == 900 + actor_index);
    unsigned char bytes[8] = {0};
    size_t length = 0;
    /* The same OS carrier cannot impersonate a different actor's turn. */
    uint64_t active_sequence = 0;
    check(ores_native_actor_scheduler_current_data_mail_sequence(
        ctx->scheduler, ctx->bindings[actor_index].actor, &active_sequence));
    check(active_sequence == 1);
    /* A mismatched source-mail ordinal cannot consume or overwrite bytes. */
    length = SIZE_MAX;
    check(ores_native_actor_bytes_try_receive_sequenced(
        ctx->bindings[actor_index].inbox,
        ctx->bindings[actor_index].storage, active_sequence + 1,
        bytes, sizeof(bytes), &length) == ORES_ACTOR_BYTES_DENIED);
    check(length == SIZE_MAX);
    check(ores_native_actor_bytes_receive_current_data(
        &ctx->bindings[1 - actor_index], bytes, sizeof(bytes), &length)
        == ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_receive_current_data(
        &ctx->bindings[actor_index], bytes, sizeof(bytes), &length)
        == ORES_ACTOR_BYTES_OK);
    check(length == 4 && bytes[0] == (unsigned char)(70 + actor_index)
          && bytes[1] == 0 && bytes[2] == 255 && bytes[3] == 10);
    atomic_fetch_add_explicit(&ctx->completed, 1, memory_order_release);
}
/* Separate scalar lane proves that Bytes-only is an explicit irreversible
 * actor admission choice, not a global break of scalar actor semantics. */
typedef struct {
    atomic_int processed;
    atomic_int incorrect_commit_calls;
} ScalarContext;
static void on_scalar(void* opaque, size_t actor_index, uint64_t payload) {
    ScalarContext* ctx = (ScalarContext*)opaque;
    check(actor_index == 0 && payload == 42);
    atomic_fetch_add_explicit(&ctx->processed, 1, memory_order_release);
}
static int must_not_commit(void* opaque, uint64_t ordinal) {
    ScalarContext* ctx = (ScalarContext*)opaque;
    (void)ordinal;
    atomic_fetch_add_explicit(
        &ctx->incorrect_commit_calls, 1, memory_order_release);
    return 1;
}
static int reject_initial_bind(void* opaque) {
    atomic_int* calls = (atomic_int*)opaque;
    atomic_fetch_add_explicit(calls, 1, memory_order_relaxed);
    return 0; /* critical negative: cannot partially change mailbox kind */
}
int main(void) {
    Context ctx = {0};
    ctx.scheduler = ores_native_actor_scheduler_open(2, 8, 2, on_data, &ctx);
    check(ctx.scheduler != NULL);
    OresNativeActorRef actors[2];
    OresNativeActorBytesInbox* inbox[2];
    for (size_t i = 0; i < 2; ++i) {
        actors[i] = ores_native_actor_scheduler_spawn(ctx.scheduler, i);
        check(actors[i].scheduler_domain != 0 && actors[i].generation != 0);
        inbox[i] = ores_native_actor_bytes_open(
            i + 1, actors[i].generation, 2, 64, 128);
        check(inbox[i] != NULL);
        check(ores_native_actor_bytes_bind_scheduler(
            &ctx.bindings[i], ctx.scheduler, actors[i], inbox[i]));
    }
    /* All scalar DATA entrypoints, including the legacy index-only
     * compatibility submit, must reject a bound Bytes-only actor. Rejecting
     * one send cannot mint a sequence or strand private inbox bytes. */
    for (size_t i = 0; i < 2; ++i) {
        check(ores_native_actor_scheduler_try_send(
            ctx.scheduler, actors[i], 0xbad)
            == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
        check(!ores_native_actor_scheduler_send(
            ctx.scheduler, actors[i], 0xbad));
        check(!ores_native_actor_scheduler_submit(
            ctx.scheduler, i, 0xbad));
        size_t pending = SIZE_MAX, used = SIZE_MAX;
        check(ores_native_actor_bytes_observe(
            inbox[i], ctx.bindings[i].storage, &pending, &used)
            == ORES_ACTOR_BYTES_OK);
        check(pending == 0 && used == 0);
    }
    /* Receiver authority cannot be exercised from test's external thread. */
    unsigned char out[8] = {0}; size_t length = 0;
    check(ores_native_actor_bytes_receive_current_data(
        &ctx.bindings[0], out, sizeof(out), &length) == ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_receive_current_data(
        &ctx.bindings[1], out, sizeof(out), &length) == ORES_ACTOR_BYTES_DENIED);
    for (size_t i = 0; i < 2; ++i) {
        unsigned char payload[4] = {(unsigned char)(70 + i), 0, 255, 10};
        size_t queued = SIZE_MAX, used = SIZE_MAX;
        /* This failure occurs AFTER scheduler credit preflight, but BEFORE
         * source-mail enqueue. No stray Bytes message or DATA ordinal. */
        check(ores_native_actor_bytes_try_send_atomic(
            &ctx.bindings[i], 900 + i, payload, 65)
            == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
        check(ores_native_actor_bytes_observe(
            inbox[i], ctx.bindings[i].storage, &queued, &used)
            == ORES_ACTOR_BYTES_OK);
        check(queued == 0 && used == 0);
        /* A valid atomic admission copies bytes, publishes the matching
         * ordinal and queues DATA before the worker can observe either. */
        check(ores_native_actor_bytes_try_send_atomic(
            &ctx.bindings[i], 900 + i, payload, sizeof(payload))
            == ORES_NATIVE_SEND_ACCEPTED);
    }
    struct timespec one_ms = {0, 1000000};
    for (unsigned i = 0; i < 3000 && atomic_load_explicit(
            &ctx.completed, memory_order_acquire) != 2; ++i)
        nanosleep(&one_ms, NULL); /* host-only test driver, never actor lane */
    check(atomic_load_explicit(&ctx.completed, memory_order_acquire) == 2);
    for (size_t i = 0; i < 2; ++i) {
        check(ores_native_actor_scheduler_stop(ctx.scheduler, actors[i]));
        check(ores_native_actor_scheduler_wait_done(ctx.scheduler, actors[i]));
        OresNativeActorTerminalState terminal = {0};
        check(ores_native_actor_scheduler_terminal_state(
            ctx.scheduler, actors[i], &terminal));
        /* The overlong rejected payload must not have consumed one DATA
         * ordinal, credit, or terminal processed count. */
        check(terminal.accepted_user_messages == 1);
        check(terminal.processed_user_messages == 1);
        check(terminal.abandoned_user_messages == 0);
        size_t pending = SIZE_MAX, used = SIZE_MAX;
        check(ores_native_actor_bytes_observe(
            inbox[i], ctx.bindings[i].storage, &pending, &used)
            == ORES_ACTOR_BYTES_OK);
        check(pending == 0 && used == 0);
        check(ores_native_actor_bytes_try_retire(
            inbox[i], ctx.bindings[i].storage) == ORES_ACTOR_BYTES_OK);
        check(ores_native_actor_bytes_try_destroy(
            inbox[i], ctx.bindings[i].storage) == ORES_ACTOR_BYTES_OK);
    }
    ores_native_actor_scheduler_retire(ctx.scheduler);
    check(ores_native_actor_scheduler_drain(ctx.scheduler));
    check(ores_native_actor_scheduler_try_destroy(ctx.scheduler));
    /* Ordinary scalar actor continues working. An unbound committed Bytes
     * path cannot invoke arbitrary commit callbacks, and bind cannot be
     * retrofitted after *any* scalar DATA was successfully admitted. */
    ScalarContext scalar = {0};
    OresNativeActorScheduler* scalar_sched =
        ores_native_actor_scheduler_open(1, 2, 1, on_scalar, &scalar);
    check(scalar_sched != NULL);
    OresNativeActorRef scalar_ref =
        ores_native_actor_scheduler_spawn(scalar_sched, 0);
    check(scalar_ref.scheduler_domain && scalar_ref.generation);
    check(ores_native_actor_scheduler_try_send_committed(
        scalar_sched, scalar_ref, 999, must_not_commit, &scalar)
        == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    check(atomic_load_explicit(
        &scalar.incorrect_commit_calls, memory_order_acquire) == 0);
    check(ores_native_actor_scheduler_try_send(
        scalar_sched, scalar_ref, 42) == ORES_NATIVE_SEND_ACCEPTED);
    OresNativeActorBytesInbox* invalid_binding =
        ores_native_actor_bytes_open(1, scalar_ref.generation, 2, 64, 128);
    check(invalid_binding != NULL);
    OresNativeActorBytesSchedulerBinding rejected = {0};
    check(!ores_native_actor_bytes_bind_scheduler(
        &rejected, scalar_sched, scalar_ref, invalid_binding));
    OresNativeActorBytesRef legacy_ref =
        ores_native_actor_bytes_ref(invalid_binding);
    /* A failed bind did NOT partially seal the private inbox either. */
    unsigned char legacy_value = 42;
    check(ores_native_actor_bytes_try_send(
        invalid_binding, legacy_ref, &legacy_value, 1) == ORES_ACTOR_BYTES_OK);
    size_t old_length = SIZE_MAX;
    unsigned char old_output = 0;
    check(ores_native_actor_bytes_try_receive(
        invalid_binding, legacy_ref, &old_output, 1, &old_length)
        == ORES_ACTOR_BYTES_OK);
    check(old_length == 1 && old_output == legacy_value);
    check(ores_native_actor_scheduler_stop(scalar_sched, scalar_ref));
    check(ores_native_actor_scheduler_wait_done(scalar_sched, scalar_ref));
    OresNativeActorTerminalState scalar_terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        scalar_sched, scalar_ref, &scalar_terminal));
    check(scalar_terminal.accepted_user_messages == 1 &&
          scalar_terminal.processed_user_messages == 1 &&
          scalar_terminal.abandoned_user_messages == 0);
    check(atomic_load_explicit(&scalar.processed, memory_order_acquire) == 1);
    check(ores_native_actor_bytes_try_retire(
        invalid_binding, legacy_ref) == ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(
        invalid_binding, legacy_ref) == ORES_ACTOR_BYTES_OK);
    ores_native_actor_scheduler_retire(scalar_sched);
    check(ores_native_actor_scheduler_drain(scalar_sched));
    check(ores_native_actor_scheduler_try_destroy(scalar_sched));

    /* A STOP that linearizes first seals both message kind entrypoints.
     * The private slab stays empty and can be physically scrubbed once
     * the scheduler has published DONE and producers are quiescent. */
    Context stopped = {0};
    stopped.scheduler = ores_native_actor_scheduler_open(
        1, 2, 1, on_data, &stopped);
    check(stopped.scheduler != NULL);
    OresNativeActorRef stopped_ref =
        ores_native_actor_scheduler_spawn(stopped.scheduler, 0);
    OresNativeActorBytesInbox* stopped_inbox =
        ores_native_actor_bytes_open(1, stopped_ref.generation, 2, 64, 128);
    check(stopped_inbox != NULL);
    atomic_int rejected_bind_calls = 0;
    check(!ores_native_actor_scheduler_bind_bytes_only(
        stopped.scheduler, stopped_ref, reject_initial_bind,
        &rejected_bind_calls));
    check(atomic_load_explicit(
        &rejected_bind_calls, memory_order_relaxed) == 1);
    check(ores_native_actor_scheduler_try_send_committed(
        stopped.scheduler, stopped_ref, 900, must_not_commit, &scalar)
        == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    check(atomic_load_explicit(
        &scalar.incorrect_commit_calls, memory_order_acquire) == 0);
    /* A second, valid bind commits the private inbox and scheduler type
     * together. Attempts to bind again must fail without resetting it. */
    check(ores_native_actor_bytes_bind_scheduler(
        &stopped.bindings[0], stopped.scheduler, stopped_ref, stopped_inbox));
    OresNativeActorBytesSchedulerBinding rejected_second_bind = {0};
    check(!ores_native_actor_bytes_bind_scheduler(
        &rejected_second_bind, stopped.scheduler, stopped_ref, stopped_inbox));
    check(ores_native_actor_scheduler_try_send(
        stopped.scheduler, stopped_ref, 900)
        == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    check(ores_native_actor_scheduler_stop_sealed(
        stopped.scheduler, stopped_ref));
    unsigned char stopped_frame[] = {70, 0, 255, 10};
    check(ores_native_actor_bytes_try_send_atomic(
        &stopped.bindings[0], 900, stopped_frame, sizeof(stopped_frame))
        == ORES_NATIVE_SEND_TERMINATED);
    check(ores_native_actor_scheduler_try_send(
        stopped.scheduler, stopped_ref, 900) == ORES_NATIVE_SEND_TERMINATED);
    check(!ores_native_actor_scheduler_submit(stopped.scheduler, 0, 900));
    check(ores_native_actor_scheduler_wait_done(
        stopped.scheduler, stopped_ref));
    size_t stopped_count = SIZE_MAX, stopped_bytes = SIZE_MAX;
    check(ores_native_actor_bytes_observe(
        stopped_inbox, stopped.bindings[0].storage,
        &stopped_count, &stopped_bytes) == ORES_ACTOR_BYTES_OK);
    check(stopped_count == 0 && stopped_bytes == 0);
    check(ores_native_actor_bytes_try_retire(
        stopped_inbox, stopped.bindings[0].storage) == ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(
        stopped_inbox, stopped.bindings[0].storage) == ORES_ACTOR_BYTES_OK);
    ores_native_actor_scheduler_retire(stopped.scheduler);
    check(ores_native_actor_scheduler_drain(stopped.scheduler));
    check(ores_native_actor_scheduler_try_destroy(stopped.scheduler));
    puts("NATIVE_ACTOR_BYTES_PHYSICAL_TURN_HOST_ONLY_PASS actors=2 current_data=2 foreign=DENIED external=DENIED atomic_admission=2 rejected_without_stray_data=2 sequence_fenced=2 mixed_scalar=DENIED scalar_lane=PASS failed_bind=ATOMIC rebind=DENIED stop_seal=PASS");
    return 0;
}
