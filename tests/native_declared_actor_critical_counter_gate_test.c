/*
 * Native LLVM runtime substrate parity for the four-file Counter contract.
 * This test executes REAL generation-fenced read/write tickets using the
 * existing C runtime. It is a host-level gate; no .atto mtd source is compiled
 * or dispatched by this test. The compiler must supply authenticated actor
 * identity, critical callback and sealed layout before enabling guest code.
 */
#include "oreslang/runtime/OresActorStateRwAdmission.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ACTOR_ID UINT64_C(42)
#define INCARNATION UINT64_C(7)
#define DECLARED (ORES_STATE_EFFECT_READ | ORES_STATE_EFFECT_WRITE)

typedef struct counter_state {
    ores_actor_state_rw permit;
    int64_t count;
} counter_state;

static ores_std_ticket started(
        counter_state* state, ores_std_access mode, unsigned effects) {
    ores_actor_state_acquisition a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, effects, mode);
    assert(a.verification == ORES_STATE_ALLOWED);
    assert(a.admission.code == ORES_STD_SYNC_GRANTED);
    assert(a.admission.ticket.scope_generation == INCARNATION);
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            a.admission.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_state_ready_to_suspend(&state->permit, ACTOR_ID, INCARNATION,
                                       a.admission.ticket)
           == ORES_STD_SYNC_EXECUTING);
    return a.admission.ticket;
}

static void finished(counter_state* state, ores_std_ticket ticket) {
    assert(ores_state_end(&state->permit, ACTOR_ID, INCARNATION,
                          ticket) == ORES_STD_SYNC_GRANTED);
    ores_std_sync_result released = ores_state_release(
        &state->permit, ACTOR_ID, INCARNATION, ticket);
    assert(released.code == ORES_STD_SYNC_GRANTED);
    assert(ores_state_ready_to_suspend(&state->permit, ACTOR_ID, INCARNATION,
                                       ticket) == ORES_STD_SYNC_GRANTED);
}

static void increment(counter_state* state) {
    ores_std_ticket lease = started(state, ORES_STD_WRITE, DECLARED);
    /* In a compiled guest this arithmetic must lower with overflow checks.
       This test deliberately stays away from the overflow boundary. */
    assert(state->count < INT64_MAX);
    ++state->count;
    finished(state, lease);
}

static int64_t get_count(counter_state* state) {
    ores_std_ticket lease = started(state, ORES_STD_READ, ORES_STATE_EFFECT_READ);
    const int64_t current = state->count;
    finished(state, lease);
    return current;
}

static void denied_access(counter_state* state) {
    ores_actor_state_acquisition a = ores_state_handle_request(
        &state->permit, ACTOR_ID + 1, INCARNATION,
        true, DECLARED, ORES_STATE_EFFECT_READ, ORES_STD_READ);
    assert(a.verification == ORES_STATE_WRONG_ACTOR);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION + 1,
        true, DECLARED, ORES_STATE_EFFECT_READ, ORES_STD_READ);
    assert(a.verification == ORES_STATE_WRONG_ACTOR);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        false, DECLARED, ORES_STATE_EFFECT_READ, ORES_STD_READ);
    assert(a.verification == ORES_STATE_NOT_CRITICAL);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, DECLARED, ORES_STD_READ);
    assert(a.verification == ORES_STATE_EFFECT_DENIED);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, ORES_STATE_EFFECT_WRITE, ORES_STD_WRITE);
    assert(a.verification == ORES_STATE_ALLOWED);
    /* A granted ticket must always be explicitly retired before proceeding. */
    assert(a.admission.code == ORES_STD_SYNC_GRANTED);
    assert(ores_state_release(&state->permit, ACTOR_ID, INCARNATION,
                              a.admission.ticket).code == ORES_STD_SYNC_GRANTED);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, ORES_STATE_EFFECT_WRITE | ORES_STATE_EFFECT_SUSPEND,
        ORES_STD_WRITE);
    assert(a.verification == ORES_STATE_EFFECT_DENIED);
    a = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, ORES_STATE_EFFECT_WRITE | ORES_STATE_EFFECT_FFI,
        ORES_STD_WRITE);
    assert(a.verification == ORES_STATE_EFFECT_DENIED);
}

static void readers_and_writer_order(counter_state* state) {
    ores_std_ticket r1 = started(state, ORES_STD_READ, ORES_STATE_EFFECT_READ);
    ores_std_ticket r2 = started(state, ORES_STD_READ, ORES_STATE_EFFECT_READ);
    ores_actor_state_acquisition queued = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, DECLARED, ORES_STD_WRITE);
    assert(queued.verification == ORES_STATE_ALLOWED);
    assert(queued.admission.code == ORES_STD_SYNC_QUEUED);
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            queued.admission.ticket) == ORES_STD_SYNC_QUEUED);
    /* A newer reader must not jump ahead of a waiting writer. */
    ores_actor_state_acquisition later = ores_state_handle_request(
        &state->permit, ACTOR_ID, INCARNATION,
        true, DECLARED, ORES_STATE_EFFECT_READ, ORES_STD_READ);
    assert(later.verification == ORES_STATE_ALLOWED);
    assert(later.admission.code == ORES_STD_SYNC_QUEUED);
    finished(state, r1);
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            queued.admission.ticket) == ORES_STD_SYNC_QUEUED);
    finished(state, r2);
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            queued.admission.ticket) == ORES_STD_SYNC_GRANTED);
    /* No readers execute while a writer owns the exclusive state permit. */
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            later.admission.ticket) == ORES_STD_SYNC_QUEUED);
    assert(ores_state_end(&state->permit, ACTOR_ID, INCARNATION,
                          queued.admission.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_state_release(&state->permit, ACTOR_ID, INCARNATION,
                              queued.admission.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_state_start(&state->permit, ACTOR_ID, INCARNATION,
                            later.admission.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_state_end(&state->permit, ACTOR_ID, INCARNATION,
                          later.admission.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_state_release(&state->permit, ACTOR_ID, INCARNATION,
                              later.admission.ticket).code == ORES_STD_SYNC_GRANTED);
}

int main(void) {
    counter_state counter = {
        .permit = ORES_ACTOR_STATE_RW_INIT(ACTOR_ID, INCARNATION),
        .count = 0
    };
    denied_access(&counter);
    assert(get_count(&counter) == 0);
    increment(&counter);
    increment(&counter);
    assert(get_count(&counter) == 2);
    readers_and_writer_order(&counter);
    assert(get_count(&counter) == 2);
    assert(ores_state_stop(&counter.permit, ACTOR_ID, INCARNATION).code
           == ORES_STD_SYNC_GRANTED);
    assert(ores_state_quiescent(&counter.permit, ACTOR_ID, INCARNATION)
           == ORES_STD_SYNC_GRANTED);
    ores_actor_state_acquisition closed = ores_state_handle_request(
        &counter.permit, ACTOR_ID, INCARNATION, true,
        DECLARED, ORES_STATE_EFFECT_READ, ORES_STD_READ);
    assert(closed.verification == ORES_STATE_ALLOWED);
    assert(closed.admission.code == ORES_STD_SYNC_CLOSED);
    assert(ores_state_start(&counter.permit, ACTOR_ID, INCARNATION,
                            (ores_std_ticket){0}) == ORES_STD_SYNC_STALE);
    puts("native critical actor counter runtime gate PASS (host-only)");
    return 0;
}
