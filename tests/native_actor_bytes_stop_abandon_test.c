#define _POSIX_C_SOURCE 200809L
/* A trusted test controller reserves the native scheduler turn gate, so
 * no actor carrier can begin DATA before STOP. This is HOST_ONLY: the actor
 * never waits on I/O or spins. Verify admitted-but-abandoned Bytes credits
 * and scrub-after-DONE, not source-level .atto/OS socket semantics. */
#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int ok) {
    if (!ok) { fputs("NATIVE_BYTES_STOP_ABANDON_FAIL\n", stderr); abort(); }
}
static void forbidden_data(void* ctx, size_t actor, uint64_t payload) {
    (void)ctx;
    (void)actor;
    (void)payload;
    /* No DATA callback may run while the external host owns the only
     * process-wide native turn permit. */
    abort();
}
int main(void) {
    OresNativePhysicalTurnGate gate = {0};
    gate.maximum = 1;
    check(ores_native_turn_gate_try_acquire(&gate)); /* test host owns gate */
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 2, 1, forbidden_data, NULL);
    check(scheduler != NULL);
    check(ores_native_actor_scheduler_configure_turn_gate(scheduler, &gate));
    OresNativeActorRef actor =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain != 0 && actor.generation != 0);
    OresNativeActorBytesInbox* inbox =
        ores_native_actor_bytes_open(1, actor.generation, 2, 8, 16);
    check(inbox != NULL);
    OresNativeActorBytesSchedulerBinding binding = {0};
    check(ores_native_actor_bytes_bind_scheduler(
        &binding, scheduler, actor, inbox));
    const unsigned char a[] = {1, 0, 255, 10};
    const unsigned char b[] = {2, 0, 255, 13};
    check(ores_native_actor_bytes_try_send_atomic(
        &binding, 101, a, sizeof(a)) == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_bytes_try_send_atomic(
        &binding, 102, b, sizeof(b)) == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_bytes_try_send_atomic(
        &binding, 103, a, sizeof(a)) == ORES_NATIVE_SEND_MAILBOX_FULL);
    check(ores_native_actor_scheduler_try_send(
        scheduler, actor, 999) == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    size_t queued = SIZE_MAX, used = SIZE_MAX;
    check(ores_native_actor_bytes_observe(
        inbox, binding.storage, &queued, &used) == ORES_ACTOR_BYTES_OK);
    check(queued == 2 && used == 8);
    /* STOP seals DATA under the same scheduler mutex as Bytes publishing.
     * Both accepted turns are abandoned, not processed/durably ACKed. */
    check(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    check(ores_native_actor_scheduler_wait_done(scheduler, actor));
    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, actor, &terminal));
    check(terminal.accepted_user_messages == 2);
    check(terminal.processed_user_messages == 0);
    check(terminal.abandoned_user_messages == 2);
    check(ores_native_actor_bytes_try_send_atomic(
        &binding, 104, a, sizeof(a)) == ORES_NATIVE_SEND_TERMINATED);
    /* Queued owned bytes remain physically reserved until their owner has
     * passed DONE. Retire only after this exact-incarnation terminal proof
     * and external producer quiescence; retirement scrubs every slot. */
    check(ores_native_actor_bytes_observe(
        inbox, binding.storage, &queued, &used) == ORES_ACTOR_BYTES_OK);
    check(queued == 2 && used == 8);
    check(ores_native_actor_bytes_try_retire(
        inbox, binding.storage) == ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_observe(
        inbox, binding.storage, &queued, &used) == ORES_ACTOR_BYTES_OK);
    check(queued == 0 && used == 0);
    check(ores_native_actor_bytes_try_destroy(
        inbox, binding.storage) == ORES_ACTOR_BYTES_OK);
    ores_native_turn_gate_release(&gate); /* test owner releases permit */
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
    puts("NATIVE_ACTOR_BYTES_STOP_ABANDON_HOST_ONLY_PASS accepted=2 processed=0 abandoned=2 queued_refunded=2 stale_send=DENIED");
    return 0;
}
