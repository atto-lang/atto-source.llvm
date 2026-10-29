/* Native scheduler + actual LLVM-compiled .ores scalar segments.
 * This is a trusted bridge regression, NOT source-level actor await lowering.
 * Host owns actor state and is responsible for physical guest frame cleanup.
 */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern int64_t guest_step(int64_t prior, int64_t payload);
extern int64_t guest_resume(int64_t prior, int64_t payload);
extern int64_t guest_cleanup(int64_t prior, int64_t reason);
extern int64_t ores_guest_fixture_main(void);

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "COMPILED_GUEST_LEDGER_FAIL %s:%d: %s\n", \
            __FILE__, __LINE__, #x); abort(); \
} } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actors[2];
    OresNativeWakeTicket wake;
    OresNativeRetainedDataTicket retained;
    int64_t guest_state[2]; /* distinct host test cells; NOT actor-owned heaps */
    atomic_int inside[2];   /* detect physical guest re-entrancy per actor */
    atomic_int suspended;
    atomic_int resumed;
    atomic_int second_ran;
    atomic_int independent_ran;
} Fixture;

static void until(atomic_int* flag) {
    for (int n = 0; n < 8000; ++n) {
        if (atomic_load_explicit(flag, memory_order_acquire)) return;
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    fputs("COMPILED_GUEST_LEDGER timeout\n", stderr);
    abort();
}
static void until_completed(Fixture* f, uint64_t target) {
    for (int n = 0; n < 8000; ++n) {
        if (ores_native_actor_scheduler_completed(f->scheduler) >= target) return;
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    fputs("COMPILED_GUEST_LEDGER completion timeout\n", stderr);
    abort();
}
static void begin_guest(Fixture* f, size_t actor) {
    CHECK(ores_native_actor_scheduler_is_current_actor(
        f->scheduler, f->actors[actor]));
    CHECK(atomic_exchange_explicit(&f->inside[actor], 1,
        memory_order_acq_rel) == 0);
}
static void end_guest(Fixture* f, size_t actor) {
    CHECK(atomic_exchange_explicit(&f->inside[actor], 0,
        memory_order_acq_rel) == 1);
}
static void data(void* raw, size_t actor, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    CHECK(actor < 2);
    begin_guest(f, actor);
    if (actor == 1) {
        CHECK(payload == 9);
        f->guest_state[1] = guest_step(f->guest_state[1], (int64_t)payload);
        CHECK(f->guest_state[1] == 9);
        atomic_store_explicit(&f->independent_ran, 1, memory_order_release);
    } else if (payload == 1) {
        CHECK(f->guest_state[0] == 0);
        f->guest_state[0] = guest_step(f->guest_state[0], (int64_t)payload);
        CHECK(f->guest_state[0] == 1);
        CHECK(ores_native_actor_scheduler_reserve_wake(
            f->scheduler, f->actors[0], f->actors[0], 1, 1, 1, 1, &f->wake));
        CHECK(ores_native_actor_scheduler_retain_current_data(
            f->scheduler, &f->retained));
        /* The native callback returns, but logical A and its mailbox credit
         * remain pending until a separately authenticated WAKE runs guest. */
        atomic_store_explicit(&f->suspended, 1, memory_order_release);
    } else if (payload == 2) {
        CHECK(atomic_load_explicit(&f->resumed, memory_order_acquire) == 1);
        CHECK(ores_native_actor_scheduler_retained_data_count(
            f->scheduler, f->actors[0]) == 0);
        CHECK(f->guest_state[0] == 114);
        f->guest_state[0] = guest_step(f->guest_state[0], (int64_t)payload);
        CHECK(f->guest_state[0] == 1256);
        atomic_store_explicit(&f->second_ran, 1, memory_order_release);
    } else {
        CHECK(0);
    }
    end_guest(f, actor);
}
static void wake_guest(void* raw, size_t actor, OresNativeWakeTicket ticket,
                       OresNativeWakeOutcome outcome) {
    Fixture* f = (Fixture*)raw;
    CHECK(actor == 0 && outcome == ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f->scheduler, ticket));
    CHECK(ticket.slot_generation == f->wake.slot_generation);
    begin_guest(f, 0);
    CHECK(atomic_load_explicit(&f->second_ran, memory_order_acquire) == 0);
    CHECK(f->guest_state[0] == 1);
    f->guest_state[0] = guest_resume(f->guest_state[0], 3);
    CHECK(f->guest_state[0] == 114);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f->scheduler, f->retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f->scheduler, f->retained));
    atomic_store_explicit(&f->resumed, 1, memory_order_release);
    end_guest(f, 0);
}
/* Retain A during its native DATA callback; another DATA admission may
 * initially see active + retained and must wait for physical callback exit.
 * Do not spin inside the scheduler carrier or release A's retained receipt. */
static void admit_second(Fixture* f) {
    for (int n = 0; n < 8000; ++n) {
        OresNativeSendStatus status = ores_native_actor_scheduler_try_send(
            f->scheduler, f->actors[0], 2);
        if (status == ORES_NATIVE_SEND_ACCEPTED) return;
        CHECK(status == ORES_NATIVE_SEND_MAILBOX_FULL);
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    fputs("COMPILED_GUEST_LEDGER B admission timeout\n", stderr);
    abort();
}
/* Separate STOP-only bridge case. A host-owned malloc frame represents
 * pinned continuation state, but the scalar cleanup *function* really comes
 * from Oreslang-generated LLVM IR. This does not imply that the compiler
 * generated the continuation, its drop glue, or language-level suspension. */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeRetainedDataTicket retained;
    int64_t* pinned_frame;
    atomic_int suspended, released, cleanup_turns;
} CompiledStopFixture;

static void compiled_stop_data(void* raw, size_t index, uint64_t payload) {
    CompiledStopFixture* f = (CompiledStopFixture*)raw;
    CHECK(index == 0 && payload == 5);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler, f->actor));
    CHECK(!f->pinned_frame);
    f->pinned_frame = (int64_t*)malloc(sizeof(*f->pinned_frame));
    CHECK(f->pinned_frame);
    *f->pinned_frame = guest_step(0, (int64_t)payload);
    CHECK(*f->pinned_frame == 5);
    CHECK(ores_native_actor_scheduler_retain_current_data(
        f->scheduler, &f->retained));
    atomic_store_explicit(&f->suspended, 1, memory_order_release);
}
static int compiled_stop_cleanup(
        void* raw, size_t index, OresNativeRetainedDataTicket ticket) {
    CompiledStopFixture* f = (CompiledStopFixture*)raw;
    CHECK(index == 0);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler, f->actor));
    CHECK(ticket.actor.scheduler_domain == f->retained.actor.scheduler_domain);
    CHECK(ticket.actor.generation == f->retained.actor.generation);
    CHECK(ticket.slot_generation == f->retained.slot_generation);
    CHECK(ticket.execution_id == f->retained.execution_id);
    CHECK(f->pinned_frame && *f->pinned_frame == 5);
    /* Generated .ores function executes inside the exclusive trusted cleanup
     * turn. A host has to certify actual physical frame release. */
    CHECK(guest_cleanup(*f->pinned_frame, 3) == 1058);
    free(f->pinned_frame);
    f->pinned_frame = NULL;
    atomic_store_explicit(&f->released, 1, memory_order_release);
    atomic_fetch_add_explicit(&f->cleanup_turns, 1, memory_order_relaxed);
    return 1; /* physical host frame has been released */
}
static void run_compiled_guest_stop_cleanup(void) {
    CompiledStopFixture f = {0};
    f.scheduler = ores_native_actor_scheduler_open(
        1, 1, 1, compiled_stop_data, &f);
    CHECK(f.scheduler);
    CHECK(ores_native_actor_scheduler_configure_retained_cleanup(
        f.scheduler, compiled_stop_cleanup));
    f.actor = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    CHECK(f.actor.scheduler_domain);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actor, 5));
    until(&f.suspended);
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler, f.actor));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.actor));
    CHECK(atomic_load_explicit(&f.released, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(&f.cleanup_turns, memory_order_acquire) == 1);
    CHECK(!f.pinned_frame);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler, f.actor) == 0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler) == 2);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler) == 1);
    CHECK(ores_native_actor_scheduler_control_completed(f.scheduler) == 0);
    OresNativeActorTerminalState terminal = {0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        f.scheduler, f.actor, &terminal));
    CHECK(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    CHECK(terminal.accepted_user_messages == 1);
    CHECK(terminal.processed_user_messages == 0);
    CHECK(terminal.abandoned_user_messages == 1);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}
int main(void) {
    CHECK(ores_guest_fixture_main() == 101);
    Fixture f = {0};
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 2, 2, data, NULL, NULL, wake_guest, &f);
    CHECK(f.scheduler);
    f.actors[0] = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    f.actors[1] = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    CHECK(f.actors[0].scheduler_domain && f.actors[1].scheduler_domain);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actors[0], 1));
    until(&f.suspended);
    admit_second(&f);
    CHECK(ores_native_actor_scheduler_try_send(f.scheduler, f.actors[0], 7)
        == ORES_NATIVE_SEND_MAILBOX_FULL);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actors[1], 9));
    until(&f.independent_ran);
    until_completed(&f, 1);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler) == 3);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler) == 1);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler, f.actors[0]) == 1);
    CHECK(!atomic_load_explicit(&f.resumed, memory_order_acquire));
    CHECK(!atomic_load_explicit(&f.second_ran, memory_order_acquire));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f.scheduler, f.retained)); /* off-actor host cannot settle */
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler, f.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, f.wake, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    until(&f.resumed);
    until(&f.second_ran);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler) == 4);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler) == 4);
    CHECK(ores_native_actor_scheduler_wakes_completed(f.scheduler) == 1);
    CHECK(f.guest_state[0] == 1256 && f.guest_state[1] == 9);
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.actors[0]));
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.actors[1]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.actors[0]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.actors[1]));
    OresNativeActorTerminalState result = {0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        f.scheduler, f.actors[0], &result));
    CHECK(result.accepted_user_messages == 2);
    CHECK(result.processed_user_messages == 2);
    CHECK(result.abandoned_user_messages == 0);
    CHECK(ores_native_actor_scheduler_terminal_state(
        f.scheduler, f.actors[1], &result));
    CHECK(result.accepted_user_messages == 1);
    CHECK(result.processed_user_messages == 1);
    CHECK(result.abandoned_user_messages == 0);
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    run_compiled_guest_stop_cleanup();
    puts("LLVM_COMPILED_ORES_SEGMENTS_RETAINED_LEDGER_PASS");
    return 0;
}
