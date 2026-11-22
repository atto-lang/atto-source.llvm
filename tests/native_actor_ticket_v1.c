/* Real native LLVM mailbox host-ticket proof; GCC/Clang -pthread.
 * The guest callback is a host stub; this does NOT prove compiled Atto actor
 * proxy syntax/ABI, recovery on panic or IPC capability enforcement.
 */
#define _POSIX_C_SOURCE 200809L
#include "../runtime/actor_ticket_v1.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern int32_t atto_actor_spawn(int32_t);
extern int32_t atto_actor_send(int32_t, int64_t);
extern void atto_actor_finish(void);

static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv = PTHREAD_COND_INITIALIZER;
static int slow_entered, slow_release;
static atomic_int slow_count, fast_count;
void atto_actor_init(int32_t type_id) { (void)type_id; }
void atto_actor_receive(int32_t type_id, int64_t value, int64_t seq) {
    (void)seq;
    if (type_id == 0 && value == 11) {
        pthread_mutex_lock(&pause_lock);
        slow_entered = 1;
        pthread_cond_broadcast(&pause_cv);
        while (!slow_release) pthread_cond_wait(&pause_cv, &pause_lock);
        pthread_mutex_unlock(&pause_lock);
    }
    if (type_id == 0) atomic_fetch_add(&slow_count, 1);
    if (type_id == 1) atomic_fetch_add(&fast_count, 1);
}
static atto_host_receipt_route_v1 scoped(int32_t ref, uint64_t id) {
    return (atto_host_receipt_route_v1){
        .protocol_major=1, .tenant=11, .caller_incarnation=12,
        .target_actor=(uint32_t)ref,
        .target_incarnation=((uint32_t)ref) >> 16,
        .association_generation=17, .placement_epoch=15,
        .message_id=id, .attempt=1
    };
}
static atto_host_ticket_state_v1 state(int32_t ref, uint64_t ticket,
                                      const atto_host_receipt_route_v1 *route) {
    atto_host_ticket_state_v1 s = 0;
    assert(atto_host_actor_ticket_snapshot(ref, ticket, route, &s) == 0);
    return s;
}
static int wait_for(int32_t ref, uint64_t ticket,
                    const atto_host_receipt_route_v1 *route,
                    atto_host_ticket_state_v1 wanted) {
    const struct timespec pause = {0, 1000000L};
    for (unsigned i = 0; i < 10000; ++i) {
        if (state(ref, ticket, route) == wanted) return 1;
        nanosleep(&pause, NULL);
    }
    return 0;
}
int main(void) {
    int32_t slow = atto_actor_spawn(0), fast = atto_actor_spawn(1);
    assert(slow > 0 && fast > 0);
    atto_host_receipt_route_v1 a = scoped(slow, 101);
    atto_host_receipt_route_v1 b = scoped(slow, 102);
    atto_host_receipt_route_v1 c = scoped(fast, 103);
    uint64_t ta=0, tb=0, tc=0;
    assert(atto_host_actor_send_ticket(slow, 11, &a, &ta) == 0 && ta != 0);
    pthread_mutex_lock(&pause_lock);
    while (!slow_entered) pthread_cond_wait(&pause_cv, &pause_lock);
    pthread_mutex_unlock(&pause_lock);
    assert(state(slow, ta, &a) == ATTO_TICKET_STARTED);
    assert(atto_host_actor_send_ticket(slow, 12, &b, &tb) == 0 && tb != 0);
    assert(state(slow, tb, &b) == ATTO_TICKET_ADMITTED);
    assert(atto_host_actor_send_ticket(fast, 70, &c, &tc) == 0 && tc != 0);
    assert(wait_for(fast, tc, &c, ATTO_TICKET_PROCESSED));
    assert(state(slow, ta, &a) == ATTO_TICKET_STARTED);
    assert(state(slow, tb, &b) == ATTO_TICKET_ADMITTED);
    atto_host_receipt_route_v1 bad = b;
    bad.target_actor = (uint32_t)fast;
    uint64_t wrongTargetTicket = 999;
    assert(atto_host_actor_send_ticket(slow, 123, &bad, &wrongTargetTicket) == -1 &&
           wrongTargetTicket == 0);
    bad = b;
    bad.tenant = 99;
    atto_host_ticket_state_v1 ignored;
    assert(atto_host_actor_ticket_snapshot(slow, tb, &bad, &ignored) == -1);
    bad = b; bad.attempt = 2;
    assert(atto_host_actor_ticket_snapshot(slow, tb, &bad, &ignored) == -1);
    bad = b; bad.target_incarnation++;
    uint64_t phantom = 999;
    assert(atto_host_actor_send_ticket(slow, 99, &bad, &phantom) == -1 &&
           phantom == 0);
    assert(atto_host_actor_ticket_snapshot(slow, tb, &bad, &ignored) == -1);
    assert(atto_host_actor_ticket_release(slow, tb, &b) == -1);
    /* Fill mailbox while first receive is still blocked. Existing two-entry
       ticket ledger stays pinned, but the normal guest local send must remain
       accepted until DATA capacity 128 and then return FULL (-2). */
    for (int i = 1; i < 128; ++i)
        assert(atto_actor_send(slow, 200 + i) == 0);
    uint64_t denied = 999;
    atto_host_receipt_route_v1 extra = scoped(slow, 200);
    assert(atto_host_actor_send_ticket(slow, 77, &extra, &denied) == -2 &&
           denied == 0);
    assert(atto_actor_send(slow, 77) == -2);
    pthread_mutex_lock(&pause_lock);
    slow_release = 1;
    pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_lock);
    assert(wait_for(slow, ta, &a, ATTO_TICKET_PROCESSED));
    assert(wait_for(slow, tb, &b, ATTO_TICKET_PROCESSED));
    assert(atto_host_actor_ticket_release(slow, ta, &a) == 0);
    assert(atto_host_actor_ticket_release(slow, tb, &b) == 0);
    assert(atto_host_actor_ticket_release(fast, tc, &c) == 0);
    assert(atto_host_actor_ticket_snapshot(slow, ta, &a, &ignored) == -1);
    assert(atto_host_actor_ticket_snapshot(fast, tc, &c, &ignored) == -1);
    atto_actor_finish(); /* joins and drains all local messages */
    assert(atomic_load(&slow_count) == 129);
    assert(atomic_load(&fast_count) == 1);
    assert(atto_actor_send(slow, 1) == -1);
    puts("ATTO_LLVM_REAL_NATIVE_MAILBOX_PER_MAIL_TICKET=PASS");
    puts("ATTO_LLVM_HOST_ONLY_NO_COMPILED_PROXY_GUEST=PASS");
    return 0;
}
