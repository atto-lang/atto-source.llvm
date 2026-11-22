/* Native actor-send tagged admission ABI integration regression (host C only).
 * Deliberately blocks the first callback so the bounded FIFO can be filled
 * deterministically, proving full-mailbox refusal without dropping messages.
 * Compiled .atto Result<Ack, SendError> remains a separate language gate.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "../runtime/actor_send_result_v1.h"

extern int32_t atto_actor_spawn(int32_t);
extern void atto_actor_finish(void);

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static int entered = 0;
static int released = 0;
static atomic_uint processed;

void atto_actor_init(int32_t type_id) {
    assert(type_id == 0);
}

void atto_actor_receive(int32_t type_id, int64_t value, int64_t sequence) {
    assert(type_id == 0);
    assert(value == 7);
    if (sequence == 0) {
        assert(pthread_mutex_lock(&gate_lock) == 0);
        entered = 1;
        assert(pthread_cond_signal(&gate_changed) == 0);
        while (!released) {
            assert(pthread_cond_wait(&gate_changed, &gate_lock) == 0);
        }
        assert(pthread_mutex_unlock(&gate_lock) == 0);
    }
    atomic_fetch_add(&processed, 1u);
}

static void expect_result(AttoActorSendResultV1 got, uint32_t tag, uint32_t reason) {
    assert(got.tag == tag);
    assert(got.reason == reason);
}

int main(void) {
    atomic_init(&processed, 0u);
    int32_t actor = atto_actor_spawn(0);
    assert(actor != 0);

    expect_result(atto_actor_send_result_v1(0, 7), ATTO_SEND_REJECTED_V1,
                  ATTO_SEND_ERROR_UNAVAILABLE_V1);
    expect_result(atto_actor_send_result_v1(actor ^ (1 << 16), 7),
                  ATTO_SEND_REJECTED_V1, ATTO_SEND_ERROR_UNAVAILABLE_V1);
    expect_result(atto_actor_send_result_v1(0x7fffffff, 7),
                  ATTO_SEND_REJECTED_V1, ATTO_SEND_ERROR_UNAVAILABLE_V1);

    expect_result(atto_actor_send_result_v1(actor, 7), ATTO_SEND_ADMITTED_V1,
                  ATTO_SEND_ERROR_NONE_V1);
    assert(pthread_mutex_lock(&gate_lock) == 0);
    while (!entered) {
        assert(pthread_cond_wait(&gate_changed, &gate_lock) == 0);
    }
    assert(pthread_mutex_unlock(&gate_lock) == 0);

    /* First message is executing (blocked); exactly 128 remain queued. */
    for (unsigned i = 0; i < 128u; ++i) {
        expect_result(atto_actor_send_result_v1(actor, 7),
                      ATTO_SEND_ADMITTED_V1, ATTO_SEND_ERROR_NONE_V1);
    }
    for (unsigned i = 0; i < 2u; ++i) {
        expect_result(atto_actor_send_result_v1(actor, 7),
                      ATTO_SEND_REJECTED_V1, ATTO_SEND_ERROR_FULL_V1);
    }
    assert(atomic_load(&processed) == 0u);

    assert(pthread_mutex_lock(&gate_lock) == 0);
    released = 1;
    assert(pthread_cond_signal(&gate_changed) == 0);
    assert(pthread_mutex_unlock(&gate_lock) == 0);
    atto_actor_finish(); /* Physical retirement after all accepted messages */

    assert(atomic_load(&processed) == 129u);
    expect_result(atto_actor_send_result_v1(actor, 7),
                  ATTO_SEND_REJECTED_V1, ATTO_SEND_ERROR_UNAVAILABLE_V1);
    puts("PASS tagged native actor send: full/stale/no-loss/drain (129 messages)");
    return 0;
}
