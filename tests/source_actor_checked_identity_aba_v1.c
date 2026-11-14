/*
 * Real SourceActorBootstrap.c generation/identity proof.
 * The TESTING-only 1-entry freelist makes the next actor reuse the exact
 * SourceActor address of the physically joined predecessor. A stale ID
 * must never resolve to the new actor, even with the same VM slot/address.
 *
 * This tests trusted compiler-owned uint64_t identity, NOT cryptographic
 * bearer capability strength or the legacy void* actor-ref API.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int constructors;

static void check(int ok) {
    if (!ok) {
        fputs("SOURCE_ACTOR_CHECKED_ID_INVARIANT_FAILED\n", stderr);
        abort();
    }
}
static void ctor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    check(heap && domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(&constructors, 1, memory_order_release);
}
static int64_t echo(OresActorI64Heap* heap, uint64_t domain,
                    uint64_t epoch, int64_t payload) {
    check(heap && domain != 0 && epoch != 0);
    return payload + 1;
}
static void until_ready(OresSourceActorId id) {
    int status;
    while ((status = oreslang_source_actor_ready_status_id(id))
            == ORES_SOURCE_ACTOR_STARTUP_PENDING ||
            status == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING)
        sched_yield();
    check(status == ORES_SOURCE_ACTOR_STARTUP_READY);
}
static void prove_stale(OresSourceActorId old) {
    check(old != 0);
    check(oreslang_source_actor_ready_status_id(old) == -1);
    check(!oreslang_source_actor_try_send_id(old, 42));
    check(!oreslang_source_actor_try_request_stop_id(old));
}
static void positive(void) {
    check(oreslang_source_actor_ready_status_id(0) == -1);
    check(oreslang_source_actor_ready_status_id(UINT64_MAX) == -1);
    check(!oreslang_source_actor_try_send_id(0, 1));
    check(!oreslang_source_actor_try_request_stop_id(UINT64_MAX));
    oreslang_source_actor_test_force_address_reuse(1);
    OresSourceActorId last = 0;
    for (unsigned i = 0; i < 128; ++i) {
        OresSourceActorId current =
            oreslang_source_actor_spawn_id(ctor, echo, 1);
        check(current != 0 && current != UINT64_MAX && current > last);
        check(oreslang_source_actor_test_address_reuses() == i);
        if (last)
            prove_stale(last);
        until_ready(current);
        check(oreslang_source_actor_try_send_id(current, i));
        if ((i & 1u) == 0) {
            oreslang_source_actor_request_stop_id(current);
            oreslang_source_actor_join_done_id(current);
        } else {
            oreslang_source_actor_stop_id(current);
        }
        prove_stale(current);
        last = current;
    }
    check(atomic_load_explicit(&constructors, memory_order_acquire) == 128);
    check(oreslang_source_actor_test_address_reuses() == 127);
    oreslang_source_actor_test_force_address_reuse(0);
    printf("SOURCE_ACTOR_CHECKED_ID_ABA_PASS|actors=128|actual_address_reuses=127|stale_status=128|stale_send=128|stale_stop=128|physical_joins=128\n");
}
static void expected_abort(const char* mode) {
    oreslang_source_actor_test_force_address_reuse(1);
    OresSourceActorId old = oreslang_source_actor_spawn_id(ctor, echo, 1);
    until_ready(old);
    oreslang_source_actor_stop_id(old);
    OresSourceActorId current = oreslang_source_actor_spawn_id(ctor, echo, 1);
    check(current != old && current > old);
    check(oreslang_source_actor_test_address_reuses() == 1);
    until_ready(current);
    prove_stale(old);
    /* Even though the current actor lives at the old physical address,
     * all fail-stop APIs MUST reject the original minted ID. */
    if (strcmp(mode, "stale-send") == 0)
        oreslang_source_actor_send_id(old, 7);
    else if (strcmp(mode, "stale-stop") == 0)
        oreslang_source_actor_request_stop_id(old);
    else if (strcmp(mode, "stale-join") == 0)
        oreslang_source_actor_join_done_id(old);
    else
        exit(64);
    fputs("SOURCE_ACTOR_CHECKED_ID_STALE_ACCEPTED\n", stderr);
    exit(17);
}
/* Compatibility-only regression: once a void* source actor has been
 * fully retired and unregistered, a subsequent host operation must not
 * dereference its freed allocation. This specifically DOES NOT prove that
 * the old pointer is safe after malloc gives that address to a new actor. */
static int reject_fully_expired_legacy_pointer(const char* mode) {
    void* actor = oreslang_source_actor_spawn(ctor, echo, 1);
    check(actor != NULL);
    while (oreslang_source_actor_ready_status(actor)
           != ORES_SOURCE_ACTOR_STARTUP_READY)
        sched_yield();
    oreslang_source_actor_stop(actor);
    check(oreslang_source_actor_ready_status(actor) == -1);
    if (strcmp(mode, "legacy-status") == 0) {
        puts("SOURCE_ACTOR_LEGACY_STALE_STATUS_REJECTED");
        return 0;
    }
    if (strcmp(mode, "legacy-send") == 0)
        oreslang_source_actor_send(actor, 5);
    else if (strcmp(mode, "legacy-stop") == 0)
        oreslang_source_actor_request_stop(actor);
    else if (strcmp(mode, "legacy-join") == 0)
        oreslang_source_actor_join_done(actor);
    else
        return 64;
    fputs("SOURCE_ACTOR_LEGACY_STALE_DEREF_ACCEPTED\n", stderr);
    return 22;
}

static void exhausted_ids_fail_closed(void) {
    oreslang_source_actor_test_advance_next_actor_id(UINT64_MAX - 1);
    const OresSourceActorId last = oreslang_source_actor_spawn_id(
        ctor, echo, 1);
    check(last == UINT64_MAX - 1);
    until_ready(last);
    oreslang_source_actor_stop_id(last);
    check(oreslang_source_actor_ready_status_id(last) == -1);
    /* Must abort before wrapping / reissuing an old identity. */
    (void)oreslang_source_actor_spawn_id(ctor, echo, 1);
    fputs("SOURCE_ACTOR_CHECKED_ID_WRAP_ACCEPTED\n", stderr);
    exit(19);
}

int main(int argc, char** argv) {
    if (argc != 2) return 64;
    if (strcmp(argv[1], "positive") == 0) {
        positive();
        return 0;
    }
    if (strcmp(argv[1], "exhaust") == 0) {
        exhausted_ids_fail_closed();
        return 20;
    }
    if (strncmp(argv[1], "legacy-", 7) == 0)
        return reject_fully_expired_legacy_pointer(argv[1]);
    expected_abort(argv[1]);
    return 18;
}
