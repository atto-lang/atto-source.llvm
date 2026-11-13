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
static int unrelated_commit_calls;
static int unrelated_commit(void* unused, uint64_t sequence) {
    (void)unused;
    (void)sequence;
    ++unrelated_commit_calls;
    return 1;
}

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
        /* Binding cannot be repeated, and any legacy scheduler DATA entry
         * would consume an ordinal without publishing its Bytes payload. */
        OresNativeActorBytesSchedulerBinding repeated = {0};
        check(!ores_native_actor_bytes_bind_scheduler(
            &repeated, ctx.scheduler, actors[i], inbox[i]));
        check(ores_native_actor_scheduler_try_send(
            ctx.scheduler, actors[i], 900 + i)
            == ORES_NATIVE_SEND_INVALID_REF);
        check(!ores_native_actor_scheduler_send(
            ctx.scheduler, actors[i], 900 + i));
        check(!ores_native_actor_scheduler_submit(ctx.scheduler, i, 900 + i));
        /* Committed-only means the one registered Bytes commit identity;
         * another host callback must not create orphan scheduler DATA. */
        check(ores_native_actor_scheduler_try_send_committed(
            ctx.scheduler, actors[i], 900 + i,
            unrelated_commit, NULL) == ORES_NATIVE_SEND_INVALID_REF);
    }
    check(unrelated_commit_calls == 0);
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
    puts("NATIVE_ACTOR_BYTES_PHYSICAL_TURN_HOST_ONLY_PASS actors=2 current_data=2 foreign=DENIED external=DENIED atomic_admission=2 rejected_without_stray_data=2 sequence_fenced=2 exclusive_payload_lanes=2 legacy_scalar_denied=6 foreign_commit_denied=2");
    return 0;
}
