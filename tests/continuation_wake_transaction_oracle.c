/*
 * Source-free failure-boundary oracle for continuation wake publication.
 * This is NOT a compiler/runtime test. Build: cc -std=c11 -Wall -Wextra
 * -Wpedantic -Werror tests/continuation_wake_transaction_oracle.c -o /tmp/wake-oracle
 * && /tmp/wake-oracle
 *
 * Contract: a committed result with reserved CONTROL admission must either
 * publish READY, or retain a durable obligation for retry/quarantine.
 * No terminal reclamation with an outstanding wake or physical frame loan.
 */
#include <assert.h>
#include <stdio.h>
typedef enum { PENDING, READY, CLAIMED, CANCELLED, QUARANTINED, DONE } State;
typedef struct {
    State state;
    int result_committed, wake_reserved, wake_pending, frame_loan;
    int retired, sticky_failure;
} Model;
static int safe(const Model *m) {
    if (m->retired && (m->wake_pending || m->frame_loan || m->sticky_failure ||
                       (m->state != DONE && m->state != CANCELLED)))
        return 0;
    if (m->state == READY && (!m->result_committed || !m->wake_reserved))
        return 0;
    if (m->state == CLAIMED && (!m->result_committed || !m->wake_reserved))
        return 0;
    if (m->result_committed && m->wake_reserved &&
        m->state == PENDING && !m->wake_pending)
        return 0;
    return 1;
}
static void scenario(int fail_reservation, int fail_publish,
                     int cancel_first, int physical_exit, int cancel_ready) {
    Model m = {0};
    m.state = PENDING;
    m.frame_loan = 1;
    assert(safe(&m));
    if (cancel_first) {
        m.state = CANCELLED;
    } else if (fail_reservation) {
        m.sticky_failure = 1;
        m.state = QUARANTINED;
    } else {
        m.wake_reserved = 1;
        m.wake_pending = 1;
        m.result_committed = 1;
        if (fail_publish) {
            /* A publication failure must preserve the outstanding wake. */
            m.sticky_failure = 1;
            m.state = QUARANTINED;
        } else {
            m.state = READY;
            m.wake_pending = 0;
        }
    }
    assert(safe(&m));
    if (m.state == READY && cancel_ready) {
        m.state = CANCELLED; /* READY loses arbitration to cancellation. */
        assert(safe(&m));
    }
    if (m.state == READY) {
        m.state = CLAIMED;
        assert(safe(&m));
        m.state = DONE;
    }
    if (physical_exit) m.frame_loan = 0;
    if (!m.frame_loan && !m.wake_pending && !m.sticky_failure &&
        (m.state == DONE || m.state == CANCELLED)) m.retired = 1;
    assert(safe(&m));
    if (m.sticky_failure || m.frame_loan || m.wake_pending)
        assert(!m.retired);
}
static void negative_controls(void) {
    Model m = {0};
    m.state = DONE; m.frame_loan = 1; m.retired = 1;
    assert(!safe(&m)); /* premature physical frame release */
    m.frame_loan = 0; m.wake_pending = 1;
    assert(!safe(&m)); /* wake still reserved */
    m.wake_pending = 0; m.sticky_failure = 1;
    assert(!safe(&m)); /* quarantined failure */
    m.sticky_failure = 0; m.state = PENDING;
    assert(!safe(&m)); /* nonterminal continuation */
    m.retired = 0; m.state = READY; m.result_committed = 0; m.wake_reserved = 1;
    assert(!safe(&m)); /* readiness without result */
    m.result_committed = 1; m.wake_reserved = 0;
    assert(!safe(&m)); /* readiness without wake reservation */
}
int main(void) {
    negative_controls();
    int count = 0;
    for (int reservation = 0; reservation < 2; ++reservation)
    for (int publication = 0; publication < 2; ++publication)
    for (int cancellation = 0; cancellation < 2; ++cancellation)
    for (int physical_exit = 0; physical_exit < 2; ++physical_exit)
    for (int cancel_ready = 0; cancel_ready < 2; ++cancel_ready) {
        scenario(reservation, publication, cancellation, physical_exit, cancel_ready);
        ++count;
    }
    printf("wake transaction oracle: %d model cases passed\n", count);
    return 0;
}
