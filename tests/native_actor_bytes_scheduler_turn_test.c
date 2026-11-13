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
    }
    /* Receiver authority cannot be exercised from test's external thread. */
    unsigned char out[8] = {0}; size_t length = 0;
    check(ores_native_actor_bytes_receive_current_data(
        &ctx.bindings[0], out, sizeof(out), &length) == ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_receive_current_data(
        &ctx.bindings[1], out, sizeof(out), &length) == ORES_ACTOR_BYTES_DENIED);
    for (size_t i = 0; i < 2; ++i) {
        unsigned char payload[4] = {(unsigned char)(70 + i), 0, 255, 10};
        check(ores_native_actor_bytes_try_send(
            inbox[i], ctx.bindings[i].storage, payload, sizeof(payload))
            == ORES_ACTOR_BYTES_OK);
    }
    /* Preloaded byte queues are NOT a unified scheduler admission contract.
     * This harness only tests physical turn authorization and private data. */
    for (size_t i = 0; i < 2; ++i)
        check(ores_native_actor_scheduler_try_send(
            ctx.scheduler, actors[i], 900 + i) == ORES_NATIVE_SEND_ACCEPTED);
    struct timespec one_ms = {0, 1000000};
    for (unsigned i = 0; i < 3000 && atomic_load_explicit(
            &ctx.completed, memory_order_acquire) != 2; ++i)
        nanosleep(&one_ms, NULL); /* host-only test driver, never actor lane */
    check(atomic_load_explicit(&ctx.completed, memory_order_acquire) == 2);
    for (size_t i = 0; i < 2; ++i) {
        check(ores_native_actor_scheduler_stop(ctx.scheduler, actors[i]));
        check(ores_native_actor_scheduler_wait_done(ctx.scheduler, actors[i]));
        check(ores_native_actor_bytes_try_retire(
            inbox[i], ctx.bindings[i].storage) == ORES_ACTOR_BYTES_OK);
        check(ores_native_actor_bytes_try_destroy(
            inbox[i], ctx.bindings[i].storage) == ORES_ACTOR_BYTES_OK);
    }
    ores_native_actor_scheduler_retire(ctx.scheduler);
    check(ores_native_actor_scheduler_drain(ctx.scheduler));
    check(ores_native_actor_scheduler_try_destroy(ctx.scheduler));
    puts("NATIVE_ACTOR_BYTES_PHYSICAL_TURN_HOST_ONLY_PASS actors=2 current_data=2 foreign=DENIED external=DENIED");
    return 0;
}
