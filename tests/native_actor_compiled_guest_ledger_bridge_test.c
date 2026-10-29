/* Native scheduler + actual LLVM-compiled .ores scalar segments.
 * This is a trusted bridge regression, NOT source-level actor await lowering.
 * Host owns actor state and is responsible for physical guest frame cleanup.
 */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
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
    /* Each actor owns a separate physically allocated zero-GC native heap.
     * The test bridge owns these pointers; guest functions receive only i64
     * values and never gain access to another actor's physical allocation. */
    OresActorI64Heap* heaps[2];
    OresActorSlot state_slots[2];
    unsigned char state_initialized[2]; /* touched only by that actor's lane */
    OresActorLoan pinned_a_loan; /* survives DATA exit until authenticated WAKE */
    atomic_int_fast64_t observed_state[2]; /* diagnostics, not authoritative state */
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
static OresActorTurn begin_guest(Fixture* f, size_t actor) {
    CHECK(ores_native_actor_scheduler_is_current_actor(
        f->scheduler, f->actors[actor]));
    CHECK(atomic_exchange_explicit(&f->inside[actor], 1,
        memory_order_acq_rel) == 0);
    /* Scheduler-owned incarnation and actor-id are mapped only inside the
     * trusted host bridge. Different actors get different heap allocations. */
    return ores_actor_i64_heap_enter(f->heaps[actor], (uint64_t)actor + 1,
        f->actors[actor].generation);
}
static void end_guest(Fixture* f, size_t actor, OresActorTurn turn) {
    ores_actor_i64_heap_leave(f->heaps[actor], turn);
    CHECK(atomic_exchange_explicit(&f->inside[actor], 0,
        memory_order_acq_rel) == 1);
}
static void data(void* raw, size_t actor, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    CHECK(actor < 2);
    OresActorTurn turn = begin_guest(f, actor);
    if (!f->state_initialized[actor]) {
        f->state_slots[actor] = ores_actor_i64_heap_new(
            f->heaps[actor], turn, 0);
        f->state_initialized[actor] = 1;
    }
    OresActorSlot state = f->state_slots[actor];
    int64_t prior = ores_actor_i64_heap_read(f->heaps[actor], turn, state);
    if (actor == 1) {
        CHECK(payload == 9);
        int64_t next = guest_step(prior, (int64_t)payload);
        CHECK(next == 9);
        ores_actor_i64_heap_write(f->heaps[actor], turn, state, next);
        atomic_store_explicit(&f->observed_state[actor], next,
            memory_order_release);
        atomic_store_explicit(&f->independent_ran, 1, memory_order_release);
    } else if (payload == 1) {
        CHECK(prior == 0);
        int64_t next = guest_step(prior, (int64_t)payload);
        CHECK(next == 1);
        ores_actor_i64_heap_write(f->heaps[actor], turn, state, next);
        atomic_store_explicit(&f->observed_state[actor], next,
            memory_order_release);
        CHECK(ores_native_actor_scheduler_reserve_wake(
            f->scheduler, f->actors[0], f->actors[0], 1, 1, 1, 1, &f->wake));
        CHECK(ores_native_actor_scheduler_retain_current_data(
            f->scheduler, &f->retained));
        /* A's physical actor-owned allocation remains loan-pinned even after
         * this carrier leaves the actor heap and DATA callback returns. */
        f->pinned_a_loan = ores_actor_i64_heap_borrow(
            f->heaps[0], turn, state);
        CHECK(f->pinned_a_loan.domain == state.domain);
        /* The native callback returns, but logical A and its mailbox credit
         * remain pending until a separately authenticated WAKE runs guest. */
        atomic_store_explicit(&f->suspended, 1, memory_order_release);
    } else if (payload == 2) {
        CHECK(atomic_load_explicit(&f->resumed, memory_order_acquire) == 1);
        CHECK(ores_native_actor_scheduler_retained_data_count(
            f->scheduler, f->actors[0]) == 0);
        CHECK(prior == 114);
        int64_t next = guest_step(prior, (int64_t)payload);
        CHECK(next == 1256);
        ores_actor_i64_heap_write(f->heaps[actor], turn, state, next);
        atomic_store_explicit(&f->observed_state[actor], next,
            memory_order_release);
        atomic_store_explicit(&f->second_ran, 1, memory_order_release);
    } else {
        CHECK(0);
    }
    end_guest(f, actor, turn);
}
static void wake_guest(void* raw, size_t actor, OresNativeWakeTicket ticket,
                       OresNativeWakeOutcome outcome) {
    Fixture* f = (Fixture*)raw;
    CHECK(actor == 0 && outcome == ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f->scheduler, ticket));
    CHECK(ticket.slot_generation == f->wake.slot_generation);
    OresActorTurn turn = begin_guest(f, 0);
    CHECK(atomic_load_explicit(&f->second_ran, memory_order_acquire) == 0);
    int64_t prior = ores_actor_i64_heap_read(
        f->heaps[0], turn, f->state_slots[0]);
    CHECK(prior == 1);
    int64_t next = guest_resume(prior, 3);
    CHECK(next == 114);
    /* The shared loan was pinned across physical DATA exit. Drop it only
     * under the authenticated WAKE lease, before any state write. */
    CHECK(f->pinned_a_loan.nonce != 0);
    ores_actor_i64_heap_release(f->heaps[0], f->pinned_a_loan);
    f->pinned_a_loan = (OresActorLoan){0};
    ores_actor_i64_heap_write(f->heaps[0], turn, f->state_slots[0], next);
    atomic_store_explicit(&f->observed_state[0], next,
        memory_order_release);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f->scheduler, f->retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f->scheduler, f->retained));
    atomic_store_explicit(&f->resumed, 1, memory_order_release);
    end_guest(f, 0, turn);
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
    f.heaps[0] = ores_actor_i64_heap_open(1, f.actors[0].generation, 4);
    f.heaps[1] = ores_actor_i64_heap_open(2, f.actors[1].generation, 4);
    CHECK(f.heaps[0] && f.heaps[1] && f.heaps[0] != f.heaps[1]);
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
    CHECK(f.pinned_a_loan.nonce != 0);
    CHECK(atomic_load_explicit(&f.observed_state[0], memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(&f.observed_state[1], memory_order_acquire) == 9);
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
    CHECK(!f.pinned_a_loan.nonce);
    CHECK(atomic_load_explicit(&f.observed_state[0], memory_order_acquire) == 1256);
    CHECK(atomic_load_explicit(&f.observed_state[1], memory_order_acquire) == 9);
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
    /* Heaps have independent allocations and independent physical teardown.
     * Actor B must survive when A's heap is reclaimed. */
    ores_actor_i64_heap_retire(f.heaps[0]);
    CHECK(ores_actor_i64_heap_try_destroy(f.heaps[0]));
    f.heaps[0] = NULL;
    CHECK(f.heaps[1] != NULL);
    ores_actor_i64_heap_retire(f.heaps[1]);
    CHECK(ores_actor_i64_heap_try_destroy(f.heaps[1]));
    f.heaps[1] = NULL;
    run_compiled_guest_stop_cleanup();
    puts("LLVM_COMPILED_ORES_SEGMENTS_RETAINED_LEDGER_PASS");
    return 0;
}
