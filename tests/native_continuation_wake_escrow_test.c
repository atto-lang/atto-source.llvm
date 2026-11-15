#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "WAKE_ESCROW_FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); \
    abort(); \
} } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef producer;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int blocked, entered, release, events[32], events_len;
    atomic_int active, errors, wakes, controls, messages;
} Fixture;

static void no_op_data(void* raw, size_t actor, uint64_t payload) {
    (void)raw; (void)actor; (void)payload;
}
static void no_op_wake(void* raw, size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    (void)raw; (void)actor; (void)ticket; (void)outcome;
}

static void event(Fixture* f, int code) {
    pthread_mutex_lock(&f->lock);
    if (f->events_len >= 32) abort();
    f->events[f->events_len++] = code;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lock);
}
static void on_data(void* raw, size_t actor, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    if (actor != 0 || !ores_native_actor_scheduler_is_current_actor(
            f->scheduler, f->producer))
        atomic_fetch_add(&f->errors, 1);
    if (atomic_fetch_add(&f->active, 1) != 0)
        atomic_fetch_add(&f->errors, 1);
    atomic_fetch_add(&f->messages, 1);
    if (payload == 1 || payload == 2) {
        pthread_mutex_lock(&f->lock);
        f->entered = (int)payload;
        pthread_cond_broadcast(&f->cv);
        while (!f->release)
            CHECK(pthread_cond_wait(&f->cv, &f->lock) == 0);
        pthread_mutex_unlock(&f->lock);
    }
    atomic_fetch_sub(&f->active, 1);
}
static void on_control(void* raw, size_t actor, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    if (actor != 0 || payload != 31 || atomic_fetch_add(&f->active, 1) != 0)
        atomic_fetch_add(&f->errors, 1);
    atomic_fetch_add(&f->controls, 1);
    event(f, 2);
    atomic_fetch_sub(&f->active, 1);
}
static void on_wake(void* raw, size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    Fixture* f = (Fixture*)raw;
    if (actor != 0 || !ores_native_actor_scheduler_is_current_wake(
            f->scheduler, ticket)
            || !ores_native_actor_scheduler_is_current_actor(
                f->scheduler, f->producer)
            || atomic_fetch_add(&f->active, 1) != 0)
        atomic_fetch_add(&f->errors, 1);
    OresNativeWakeTicket forged = ticket;
    forged.continuation_generation++;
    if (ores_native_actor_scheduler_is_current_wake(f->scheduler, forged))
        atomic_fetch_add(&f->errors, 1);
    if (outcome != ORES_NATIVE_WAKE_READY
            && outcome != ORES_NATIVE_WAKE_CANCELLED
            && outcome != ORES_NATIVE_WAKE_FAILED)
        atomic_fetch_add(&f->errors, 1);
    atomic_fetch_add(&f->wakes, 1);
    event(f, 1);
    atomic_fetch_sub(&f->active, 1);
}
static void wait_entered(Fixture* f, int value) {
    pthread_mutex_lock(&f->lock);
    while (f->entered != value)
        CHECK(pthread_cond_wait(&f->cv, &f->lock) == 0);
    pthread_mutex_unlock(&f->lock);
}
static OresNativeWakeTicket reserve(Fixture* f, uint64_t descriptor) {
    OresNativeWakeTicket ticket = {0};
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f->scheduler, f->producer, f->producer,
        descriptor, 5, 7, descriptor, &ticket));
    CHECK(ticket.producer.generation == f->producer.generation);
    CHECK(ticket.target.generation == f->producer.generation);
    CHECK(ticket.continuation_slot == descriptor);
    return ticket;
}
static void setup(Fixture* f) {
    CHECK(pthread_mutex_init(&f->lock, NULL) == 0);
    CHECK(pthread_cond_init(&f->cv, NULL) == 0);
    f->scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 4, 1, on_data, on_control, NULL, on_wake, f);
    CHECK(f->scheduler);
    f->producer = ores_native_actor_scheduler_spawn(f->scheduler, 0);
    CHECK(f->producer.scheduler_domain);
    CHECK(!ores_native_actor_scheduler_is_current_wake(
        f->scheduler, (OresNativeWakeTicket){0}));
}
static void teardown(Fixture* f) {
    CHECK(ores_native_actor_scheduler_drain(f->scheduler));
    CHECK(ores_native_actor_scheduler_wakes_completed(f->scheduler)
        == (uint64_t)atomic_load(&f->wakes));
    CHECK(!atomic_load(&f->errors));
    ores_native_actor_scheduler_retire(f->scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f->scheduler));
    CHECK(pthread_cond_destroy(&f->cv) == 0);
    CHECK(pthread_mutex_destroy(&f->lock) == 0);
}

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeWakeTicket ticket;
    atomic_int* go;
    int transfer, won;
} Contender;
static void* contend(void* raw) {
    Contender* c = (Contender*)raw;
    while (!atomic_load_explicit(c->go, memory_order_acquire)) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    c->won = c->transfer
        ? ores_native_actor_scheduler_transfer_wake(c->scheduler, c->ticket)
        : ores_native_actor_scheduler_cancel_wake(c->scheduler, c->ticket);
    return NULL;
}
static void contest(Fixture* f, OresNativeWakeTicket ticket, int both_cancel) {
    atomic_int go = 0;
    Contender a = {f->scheduler, ticket, &go, 0, 0};
    Contender b = {f->scheduler, ticket, &go, !both_cancel, 0};
    pthread_t first, second;
    CHECK(pthread_create(&first, NULL, contend, &a) == 0);
    CHECK(pthread_create(&second, NULL, contend, &b) == 0);
    atomic_store_explicit(&go, 1, memory_order_release);
    CHECK(pthread_join(first, NULL) == 0);
    CHECK(pthread_join(second, NULL) == 0);
    CHECK(a.won + b.won == 1);
    CHECK(!ores_native_actor_scheduler_cancel_wake(f->scheduler, ticket));
    if (b.won && b.transfer) {
        CHECK(ores_native_actor_scheduler_abort_transferred_wake(
            f->scheduler, ticket));
    } else {
        /* A cancel winner owns no transferred wake to abort. */
        CHECK(!ores_native_actor_scheduler_abort_transferred_wake(
            f->scheduler, ticket));
    }
}
int main(void) {
    /* Public wake-ticket APIs must fail closed on legacy schedulers that do
     * not allocate a wake table. Before the matching_wake_locked hardening,
     * a valid producer ref could drive transfer/cancel/publish into a NULL
     * lane->wakes dereference. */
    OresNativeActorScheduler* plain = ores_native_actor_scheduler_open(
        1, 1, 1, no_op_data, NULL);
    CHECK(plain);
    OresNativeActorRef plain_actor = ores_native_actor_scheduler_spawn(plain, 0);
    CHECK(plain_actor.scheduler_domain);
    OresNativeWakeTicket unsupported = {
        .producer = plain_actor,
        .target = plain_actor,
        .slot = 0,
        .slot_generation = 1,
        .continuation_slot = 1,
        .continuation_generation = 1,
        .code_generation = 1,
        .descriptor_generation = 1
    };
    CHECK(!ores_native_actor_scheduler_transfer_wake(plain, unsupported));
    CHECK(!ores_native_actor_scheduler_cancel_wake(plain, unsupported));
    CHECK(!ores_native_actor_scheduler_abort_transferred_wake(plain, unsupported));
    CHECK(ores_native_actor_scheduler_publish_wake(
        plain, unsupported, ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        plain, plain_actor) == 0);
    ores_native_actor_scheduler_retire(plain);
    CHECK(ores_native_actor_scheduler_try_destroy(plain));

    /* A local target that stops after reservation must lose before the
     * irreversible transfer boundary. The reserved producer ticket remains
     * cancellable and cannot become a false mailbox-admission receipt. */
    OresNativeActorScheduler* local = ores_native_actor_scheduler_open_with_wakes(
        2, 1, 1, no_op_data, NULL, NULL, no_op_wake, NULL);
    CHECK(local);
    OresNativeActorRef producer = ores_native_actor_scheduler_spawn(local, 0);
    OresNativeActorRef target = ores_native_actor_scheduler_spawn(local, 1);
    CHECK(producer.scheduler_domain && target.scheduler_domain);
    OresNativeWakeTicket target_ticket = {0};
    CHECK(ores_native_actor_scheduler_reserve_wake(
        local, producer, target, 1, 1, 1, 1, &target_ticket));
    CHECK(ores_native_actor_scheduler_stop(local, target));
    CHECK(!ores_native_actor_scheduler_transfer_wake(local, target_ticket));
    CHECK(ores_native_actor_scheduler_cancel_wake(local, target_ticket));
    CHECK(ores_native_actor_scheduler_stop(local, producer));
    CHECK(ores_native_actor_scheduler_wait_done(local, target));
    CHECK(ores_native_actor_scheduler_wait_done(local, producer));
    ores_native_actor_scheduler_retire(local);
    CHECK(ores_native_actor_scheduler_try_destroy(local));

    Fixture f = {0};
    setup(&f);
    /* DATA holds the actor's exclusive physical turn. Wake admission
     * prestarts the single worker but cannot run a callback concurrently. */
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.producer, 1));
    wait_entered(&f, 1);
    OresNativeWakeTicket tickets[ORES_NATIVE_WAKE_CREDITS_PER_ACTOR];
    for (size_t i = 0; i < ORES_NATIVE_WAKE_CREDITS_PER_ACTOR; ++i)
        tickets[i] = reserve(&f, i + 11);
    CHECK(ores_native_actor_scheduler_started_carriers(f.scheduler) == 1);
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.producer) == ORES_NATIVE_WAKE_CREDITS_PER_ACTOR);
    OresNativeWakeTicket failed = {0};
    CHECK(!ores_native_actor_scheduler_reserve_wake(f.scheduler,
        f.producer, f.producer, 200, 5, 7, 200, &failed));
    CHECK(!failed.slot_generation);

    /* Saturated WAKE credits do not borrow or block supervisor CONTROL. */
    CHECK(ores_native_actor_scheduler_publish_control(
        f.scheduler, f.producer, 31));
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler, tickets[0]));
    CHECK(!ores_native_actor_scheduler_cancel_wake(f.scheduler, tickets[0]));
    CHECK(!ores_native_actor_scheduler_transfer_wake(f.scheduler, tickets[0]));
    OresNativeWakeTicket forged = tickets[0];
    forged.code_generation++;
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, forged, ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, tickets[0], ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_PUBLISHED);
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, tickets[0], ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_actor_scheduler_cancel_wake(f.scheduler, tickets[1]));
    CHECK(!ores_native_actor_scheduler_cancel_wake(f.scheduler, tickets[1]));
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler, tickets[2]));
    CHECK(ores_native_actor_scheduler_abort_transferred_wake(
        f.scheduler, tickets[2]));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, tickets[2], ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_INVALID);
    for (size_t i = 3; i < ORES_NATIVE_WAKE_CREDITS_PER_ACTOR; ++i)
        CHECK(ores_native_actor_scheduler_cancel_wake(f.scheduler, tickets[i]));
    pthread_mutex_lock(&f.lock);
    f.release = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.lock);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(atomic_load(&f.controls) == 1);
    CHECK(atomic_load(&f.wakes) == 1);
    CHECK(f.events_len == 2 && f.events[0] == 2 && f.events[1] == 1);
    CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.producer));
    /* ABA: freed slot is reused with a different generation. */
    OresNativeWakeTicket newer = reserve(&f, 25);
    CHECK(newer.slot == tickets[0].slot);
    CHECK(newer.slot_generation != tickets[0].slot_generation);
    CHECK(!ores_native_actor_scheduler_cancel_wake(f.scheduler, tickets[0]));
    CHECK(ores_native_actor_scheduler_cancel_wake(f.scheduler, newer));
    /* Racing cancel/cancel and cancel/transfer have a single winner. */
    for (int i = 0; i < 100; ++i) {
        OresNativeWakeTicket t = reserve(&f, 100 + (uint64_t)i);
        contest(&f, t, i % 2);
    }

    /* STOP after irrevocable transfer preserves physical DONE fencing.
     * Once STOP wins, publication consumes the credit without guest resume. */
    pthread_mutex_lock(&f.lock);
    f.entered = 0;
    f.release = 0;
    pthread_mutex_unlock(&f.lock);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.producer, 2));
    wait_entered(&f, 2);
    OresNativeWakeTicket pending = reserve(&f, 800);
    OresNativeWakeTicket transferred = reserve(&f, 801);
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler, transferred));
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler, f.producer));
    CHECK(!ores_native_actor_scheduler_transfer_wake(f.scheduler, pending));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, transferred, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PRODUCER_RETIRED);
    CHECK(ores_native_actor_scheduler_cancel_wake(f.scheduler, pending));
    pthread_mutex_lock(&f.lock);
    f.release = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.lock);
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_poll_done(f.scheduler, f.producer));
    CHECK(atomic_load(&f.wakes) == 1);
    CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.producer));
    CHECK(!ores_native_actor_scheduler_reserve_wake(f.scheduler, f.producer,
        f.producer, 999, 5, 7, 999, &failed));
    teardown(&f);
    puts("NATIVE_CONTINUATION_WAKE_ESCROW_PASS");
    return 0;
}
