/* Native scheduler admission proof for two exact actor incarnations.
 * Host-only; never a guest-callable capability.
 */
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned cases=0;
static void check(const char* name, int ok) {
    if (!ok) {
        fprintf(stderr,"DUAL_ACTOR_COMMIT_FAIL %s\n",name);
        exit(19);
    }
    ++cases;
    printf("DUAL_ACTOR_COMMIT_PASS %s\n",name);
}
static void noop(void* ctx, size_t actor, uint64_t value) {
    (void)ctx; (void)actor; (void)value;
}
static int count_commit(void* opaque) {
    unsigned* count=(unsigned*)opaque;
    ++*count;
    return 1;
}
static int reject_commit(void* opaque) {
    unsigned* count=(unsigned*)opaque;
    ++*count;
    return 0;
}
int main(void) {
    OresNativeActorScheduler* s=ores_native_actor_scheduler_open(3,8,2,noop,NULL);
    if (!s) return 2;
    const OresNativeActorRef a=ores_native_actor_scheduler_spawn(s,0);
    const OresNativeActorRef b=ores_native_actor_scheduler_spawn(s,1);
    const OresNativeActorRef c=ores_native_actor_scheduler_spawn(s,2);
    unsigned invocations=0;
    check("01_live_pair_admits",
          ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,b,count_commit,&invocations) && invocations==1);
    check("02_reversed_pair_admits",
          ores_native_actor_scheduler_commit_if_both_live_v1(
              s,b,a,count_commit,&invocations) && invocations==2);
    check("03_same_generation_pair_admits",
          ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,a,count_commit,&invocations) && invocations==3);
    check("04_null_commit_rejected",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,b,NULL,&invocations) && invocations==3);
    check("05_null_scheduler_rejected",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              NULL,a,b,count_commit,&invocations) && invocations==3);
    OresNativeActorRef stale=b; ++stale.generation;
    check("06_stale_recipient_generation_rejected",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,stale,count_commit,&invocations) && invocations==3);
    OresNativeActorRef foreign=b; ++foreign.scheduler_domain;
    check("07_foreign_scheduler_domain_rejected",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,foreign,count_commit,&invocations) && invocations==3);
    OresNativeActorRef invalid=b; invalid.actor_slot=99;
    check("08_unspawned_recipient_slot_rejected",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,invalid,count_commit,&invocations) && invocations==3);
    check("09_callback_failure_does_not_claim_success",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,b,reject_commit,&invocations) && invocations==4);
    check("10_stop_recipient_sealed",
          ores_native_actor_scheduler_stop_sealed(s,b));
    check("11_stopped_recipient_fences_callback",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,b,count_commit,&invocations) && invocations==4);
    check("12_reversed_stopped_sender_fenced",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,b,a,count_commit,&invocations) && invocations==4);
    check("13_live_sibling_pair_unaffected",
          ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,c,count_commit,&invocations) && invocations==5);
    check("14_stop_sender_sealed",ores_native_actor_scheduler_stop_sealed(s,a));
    check("15_stopped_sender_fences_remaining_pair",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,a,c,count_commit,&invocations) && invocations==5);
    ores_native_actor_scheduler_retire(s);
    check("16_runtime_retirement_rejects_any_commit",
          !ores_native_actor_scheduler_commit_if_both_live_v1(
              s,c,c,count_commit,&invocations) && invocations==5);
    if (!ores_native_actor_scheduler_wait_done(s,a) ||
        !ores_native_actor_scheduler_wait_done(s,b) ||
        !ores_native_actor_scheduler_wait_done(s,c) ||
        !ores_native_actor_scheduler_drain(s) ||
        !ores_native_actor_scheduler_try_destroy(s)) return 3;
    if (cases!=16) return 4;
    puts("NATIVE_DUAL_ACTOR_COMMIT_16_C_CASES_PASS");
    return 0;
}
