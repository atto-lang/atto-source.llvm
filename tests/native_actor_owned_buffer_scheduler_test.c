#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>

typedef struct {
    OresNativeActorScheduler* vm;
    OresNativeActorRef refs[3];
    atomic_int entered;
    atomic_int unblock;
    atomic_int messages;
    atomic_int delivered;
    atomic_int discarded;
    atomic_int commits;
    atomic_int invalid;
    atomic_int result;
    atomic_int errors;
} Fixture;

static void check(int ok) { if (!ok) abort(); }
static void wait_for(atomic_int* value, int target) {
    for (unsigned i = 0; i < 5000000; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) >= target) return;
        sched_yield();
    }
    abort();
}
static int commit_buffer(void* ctx, OresNativeActorRef sender,
                         OresNativeActorRef target,
                         OresNativeOwnedBufferTicket* ticket) {
    Fixture* s = (Fixture*)ctx;
    if (sender.actor_slot != 0 || target.actor_slot == 0
            || ticket->tenant_epoch != 7 || ticket->type_schema != 29
            || ticket->buffer_generation != 1)
        atomic_fetch_add(&s->errors, 1);
    if (ticket->buffer_id == 4) return 0; /* failed take does not publish */
    ticket->buffer_generation = 2; /* mock committed generation, not a pool */
    atomic_fetch_add(&s->commits, 1);
    return 1;
}
static void deliver_buffer(void* ctx, OresNativeActorRef receiver,
                           OresNativeOwnedBufferTicket ticket) {
    Fixture* s = (Fixture*)ctx;
    if (!ores_native_actor_scheduler_is_current_actor(s->vm, s->refs[receiver.actor_slot])
            || receiver.actor_slot != 2
            || receiver.generation != s->refs[2].generation
            || ticket.buffer_generation != 2
            || ticket.buffer_id != 2)
        atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->delivered, 1);
}
static void discard_buffer(void* ctx, OresNativeActorRef receiver,
                           OresNativeOwnedBufferTicket ticket) {
    Fixture* s = (Fixture*)ctx;
    if (receiver.actor_slot != 1
            || receiver.generation != s->refs[1].generation
            || ticket.buffer_id != 1
            || ticket.buffer_generation != 2
            || ores_native_actor_scheduler_is_current_carrier(s->vm))
        atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->discarded, 1);
}
static void on_scalar(void* ctx, size_t actor, uint64_t value) {
    Fixture* s = (Fixture*)ctx;
    if (actor == 1) {
        atomic_store_explicit(&s->entered, 1, memory_order_release);
        while (!atomic_load_explicit(&s->unblock, memory_order_acquire))
            sched_yield();
        return;
    }
    if (actor != 0) {
        atomic_fetch_add(&s->errors, 1);
        return;
    }
    OresNativeOwnedBufferTicket ticket = {7,29,value,1};
    OresNativeActorRef dst = s->refs[value == 2 || value == 4 ? 2 : 1];
    OresNativeSendStatus outcome =
        ores_native_actor_scheduler_try_send_owned_buffer(
            s->vm, s->refs[0], dst, ticket);
    atomic_store_explicit(&s->result, (int)outcome, memory_order_release);
    atomic_fetch_add_explicit(&s->messages, 1, memory_order_release);
}
int main(void) {
    Fixture s = {0};
    s.vm = ores_native_actor_scheduler_open(3, 2, 3, on_scalar, &s);
    check(s.vm != NULL);
    check(ores_native_actor_scheduler_configure_owned_buffer(
        s.vm, commit_buffer, deliver_buffer, discard_buffer));
    for (size_t i = 0; i < 3; ++i) {
        s.refs[i] = ores_native_actor_scheduler_spawn(s.vm, i);
        check(s.refs[i].scheduler_domain != 0);
    }
    check(!ores_native_actor_scheduler_configure_owned_buffer(
        s.vm, commit_buffer, deliver_buffer, discard_buffer));
    const OresNativeOwnedBufferTicket ticket = {7,29,99,1};
    check(ores_native_actor_scheduler_try_send_owned_buffer(
        s.vm, s.refs[0], s.refs[1], ticket) == ORES_NATIVE_SEND_INVALID_REF);
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[1],999) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.entered,1);

    /* First typed packet is accepted behind an active scalar DATA turn. */
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[0],1) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.messages,1);
    check(atomic_load(&s.result) == ORES_NATIVE_SEND_ACCEPTED);
    check(atomic_load(&s.commits) == 1);
    /* Capacity includes the current scalar turn and one queued buffer. */
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[0],3) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.messages,2);
    check(atomic_load(&s.result) == ORES_NATIVE_SEND_MAILBOX_FULL);
    check(atomic_load(&s.commits) == 1);

    /* Sealed STOP drops the accepted buffer once, outside scheduler lock. */
    check(ores_native_actor_scheduler_stop_sealed(s.vm,s.refs[1]));
    check(atomic_load(&s.discarded) == 1);
    check(atomic_load(&s.delivered) == 0);
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[0],3) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.messages,3);
    check(atomic_load(&s.result) == ORES_NATIVE_SEND_TERMINATED);
    check(atomic_load(&s.commits) == 1);
    atomic_store(&s.unblock,1);
    check(ores_native_actor_scheduler_wait_done(s.vm,s.refs[1]));
    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        s.vm,s.refs[1],&terminal));
    check(terminal.accepted_user_messages == 2);
    check(terminal.processed_user_messages == 1);
    check(terminal.abandoned_user_messages == 1);

    /* Same FIFO handles typed delivery with a distinct non-scalar callback. */
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[0],2) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.messages,4);
    check(atomic_load(&s.result) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.delivered,1);
    check(atomic_load(&s.commits) == 2);

    /* Failed host ownership commit cannot enqueue or change admission state. */
    check(ores_native_actor_scheduler_try_send(
        s.vm,s.refs[0],4) == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.messages,5);
    check(atomic_load(&s.result) == ORES_NATIVE_SEND_OWNERSHIP_REJECTED);
    check(atomic_load(&s.commits) == 2);
    check(atomic_load(&s.errors) == 0);

    ores_native_actor_scheduler_retire(s.vm);
    check(ores_native_actor_scheduler_drain(s.vm));
    check(ores_native_actor_scheduler_try_destroy(s.vm));
    puts("NATIVE_TYPED_BUFFER_SCHEDULER_HOST_PASS accepted=2 delivered=1 abandoned=1");
    return 0;
}
