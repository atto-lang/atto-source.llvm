/* C-level STOP notification contract; no RequestLocal or guest source.
 * The callback performs ONLY an atomic increment under scheduler mutex.
 * A mutation removing all scheduler notice invocations MUST fail this test.
 */
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int count = 0;
static void check(const char* name, int ok) {
    if (!ok) {
        fprintf(stderr, "STOP_NOTICE_CASE_FAIL %s\n", name);
        exit(13);
    }
    ++count;
    printf("STOP_NOTICE_CASE_PASS %s\n", name);
}
static void on_message(void* opaque, size_t index, uint64_t data) {
    (void)opaque; (void)index; (void)data;
}
static void notice(void* opaque) {
    _Atomic unsigned* observed = (_Atomic unsigned*)opaque;
    atomic_fetch_add_explicit(observed, 1u, memory_order_relaxed);
}
int main(void) {
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open(
        2u, 8u, 2u, on_message, NULL);
    if (!s) return 2;
    const OresNativeActorRef a = ores_native_actor_scheduler_spawn(s, 0u);
    const OresNativeActorRef b = ores_native_actor_scheduler_spawn(s, 1u);
    _Atomic unsigned a_seen = 0, b_seen = 0, invalid_seen = 0;
    OresNativeActorRef stale = a;
    ++stale.generation;
    OresNativeActorRef foreign = a;
    ++foreign.scheduler_domain;
    OresNativeActorRef wrong_slot = a;
    wrong_slot.actor_slot = 99u;

    check("01_register_current_actor",
          ores_native_actor_scheduler_register_stop_notice_v1(
              s,a,notice,&a_seen));
    check("02_duplicate_registration_fails",
          !ores_native_actor_scheduler_register_stop_notice_v1(
              s,a,notice,&invalid_seen));
    check("03_stale_generation_fails",
          !ores_native_actor_scheduler_register_stop_notice_v1(
              s,stale,notice,&invalid_seen));
    check("04_foreign_domain_fails",
          !ores_native_actor_scheduler_register_stop_notice_v1(
              s,foreign,notice,&invalid_seen));
    check("05_invalid_slot_fails",
          !ores_native_actor_scheduler_register_stop_notice_v1(
              s,wrong_slot,notice,&invalid_seen));
    check("06_wrong_listener_unregistration_fails",
          !ores_native_actor_scheduler_unregister_stop_notice_v1(
              s,a,notice,&invalid_seen));
    check("07_sealed_STOP_admitted",
          ores_native_actor_scheduler_stop_sealed(s,a));
    check("08_exactly_once_notification_on_STOP",
          atomic_load_explicit(&a_seen,memory_order_relaxed)==1u);
    check("09_duplicate_STOP_denied",
          !ores_native_actor_scheduler_stop(s,a));
    check("10_NO_duplicate_notification",
          atomic_load_explicit(&a_seen,memory_order_relaxed)==1u);
    check("11_unregister_after_STOP",
          ores_native_actor_scheduler_unregister_stop_notice_v1(
              s,a,notice,&a_seen));
    check("12_stopped_actor_cannot_register",
          !ores_native_actor_scheduler_register_stop_notice_v1(
              s,a,notice,&invalid_seen));
    check("13_second_actor_registers",
          ores_native_actor_scheduler_register_stop_notice_v1(
              s,b,notice,&b_seen));
    ores_native_actor_scheduler_retire(s);
    check("14_runtime_retire_notifies_second_actor",
          atomic_load_explicit(&b_seen,memory_order_relaxed)==1u);
    ores_native_actor_scheduler_retire(s);
    check("15_repeated_retire_sends_no_duplicate",
          atomic_load_explicit(&b_seen,memory_order_relaxed)==1u);
    if (!ores_native_actor_scheduler_wait_done(s,a) ||
        !ores_native_actor_scheduler_wait_done(s,b)) return 3;
    check("16_observer_blocks_scheduler_destroy_even_after_DONE",
          !ores_native_actor_scheduler_try_destroy(s));
    check("17_unregister_after_runtime_retire",
          ores_native_actor_scheduler_unregister_stop_notice_v1(
              s,b,notice,&b_seen));
    if (!ores_native_actor_scheduler_drain(s) ||
        !ores_native_actor_scheduler_try_destroy(s)) return 4;
    if (count != 17) return 5;
    puts("NATIVE_ACTOR_STOP_NOTICE_17_C_CASES_PASS");
    return 0;
}
