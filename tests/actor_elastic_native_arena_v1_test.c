#include "oreslang/runtime/ActorElasticNativeArenaV1.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#define KiB 1024u
static void test_peak_quota_and_actor_survival(void) {
    OresElasticNativeBudgetV1 tenant,process;
    assert(ores_elastic_budget_init_v1(&tenant,16*KiB));
    assert(ores_elastic_budget_init_v1(&process,16*KiB));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(1,1,8*KiB,8,&tenant,&process);
    OresElasticNativeArenaV1* b=ores_elastic_native_open_v1(2,1,8*KiB,8,&tenant,&process);
    assert(a && b && !ores_elastic_native_committed_v1(a));
    OresElasticNativeHandleV1 root_a,root_b,tail;
    assert(ores_elastic_native_allocate_v1(a,64,8,&root_a));
    assert(ores_elastic_native_allocate_v1(b,64,8,&root_b));
    assert(root_a.word==root_b.word && root_a.domain!=root_b.domain);
    assert(ores_elastic_native_write_i64_v1(a,root_a,0,55));
    assert(ores_elastic_native_write_i64_v1(b,root_b,0,77));
    assert(ores_elastic_budget_used_v1(&process)==8*KiB);
    assert(ores_elastic_native_allocate_v1(a,4*KiB,8,&tail));
    assert(ores_elastic_native_committed_v1(a)==8*KiB);
    assert(ores_elastic_budget_used_v1(&tenant)==12*KiB);
    assert(!ores_elastic_native_allocate_v1(b,4*KiB,8,&tail));
    assert(ores_elastic_native_committed_v1(b)==4*KiB);
    int64_t x=0;
    assert(ores_elastic_native_read_i64_v1(a,root_a,0,&x) && x==55);
    assert(ores_elastic_native_read_i64_v1(b,root_b,0,&x) && x==77);
    assert(!ores_elastic_native_read_i64_v1(b,root_a,0,&x));
    assert(ores_elastic_native_drop_v1(a,root_a));
    assert(!ores_elastic_native_read_i64_v1(a,root_a,0,&x));
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&process)==4*KiB);
    assert(ores_elastic_native_read_i64_v1(b,root_b,0,&x) && x==77);
    ores_elastic_native_close_v1(b);
    assert(!ores_elastic_budget_used_v1(&process));
    assert(!ores_elastic_budget_used_v1(&tenant));
}
static void test_relocation_and_injected_failure(void) {
    OresElasticNativeBudgetV1 both;
    assert(ores_elastic_budget_init_v1(&both,12*KiB));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(10,2,8*KiB,4,&both,&both);
    assert(a);
    OresElasticNativeHandleV1 root,second;
    assert(ores_elastic_native_allocate_v1(a,4096,8,&root));
    assert(ores_elastic_native_write_i64_v1(a,root,16,0x112233445566ll));
    ores_elastic_native_fail_next_growth_for_test_v1(a);
    assert(!ores_elastic_native_allocate_v1(a,4096,8,&second));
    assert(ores_elastic_budget_used_v1(&both)==4096 && ores_elastic_native_live_v1(a)==1);
    assert(ores_elastic_native_allocate_v1(a,4096,8,&second));
    int64_t x=0;
    assert(ores_elastic_native_read_i64_v1(a,root,16,&x) && x==0x112233445566ll);
    assert(ores_elastic_native_read_i64_v1(a,second,0,&x) && x==0);
    assert(ores_elastic_budget_used_v1(&both)==8192);
    assert(ores_elastic_native_drop_v1(a,second));
    assert(ores_elastic_native_drop_v1(a,root));
    assert(!ores_elastic_native_drop_v1(a,root));
    OresElasticNativeHandleV1 replacement;
    assert(ores_elastic_native_allocate_v1(a,8,8,&replacement));
    assert(replacement.word==1 && replacement.generation!=root.generation);
    assert(ores_elastic_native_read_i64_v1(a,replacement,0,&x) && x==0);
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&both)==0);
}
static void test_peak_limit_rejects_relocation(void) {
    OresElasticNativeBudgetV1 both;
    assert(ores_elastic_budget_init_v1(&both,8*KiB));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(20,1,8*KiB,4,&both,&both);
    assert(a);
    OresElasticNativeHandleV1 root,tail;
    assert(ores_elastic_native_allocate_v1(a,4*KiB,8,&root));
    assert(!ores_elastic_native_allocate_v1(a,4*KiB,8,&tail));
    assert(ores_elastic_native_committed_v1(a)==4*KiB);
    assert(ores_elastic_budget_used_v1(&both)==4*KiB);
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&both)==0);
}
static void test_stale_slot_offset_collision_denied(void) {
    OresElasticNativeBudgetV1 budget;
    assert(ores_elastic_budget_init_v1(&budget,12*KiB));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(99,1,1024,3,&budget,&budget);
    assert(a);
    OresElasticNativeHandleV1 first, stale, second, third, fourth;
    assert(ores_elastic_native_allocate_v1(a,8,8,&first));
    assert(ores_elastic_native_allocate_v1(a,8,64,&stale));
    assert(stale.word==9 && stale.slot==1 && stale.generation==1);
    assert(ores_elastic_native_drop_v1(a,first));
    assert(ores_elastic_native_drop_v1(a,stale));
    assert(ores_elastic_native_allocate_v1(a,8,8,&second));
    assert(ores_elastic_native_allocate_v1(a,8,8,&third));
    assert(ores_elastic_native_allocate_v1(a,8,64,&fourth));
    assert(fourth.word==stale.word && fourth.generation==stale.generation);
    assert(fourth.slot!=stale.slot);
    int64_t got=0;
    assert(ores_elastic_native_write_i64_v1(a,fourth,0,123));
    assert(!ores_elastic_native_read_i64_v1(a,stale,0,&got));
    assert(ores_elastic_native_read_i64_v1(a,fourth,0,&got) && got==123);
    ores_elastic_native_close_v1(a);
    assert(!ores_elastic_budget_used_v1(&budget));
}
typedef struct {
    OresElasticNativeBudgetV1* tenant;
    OresElasticNativeBudgetV1* process;
    unsigned id;
    int success;
} ThreadCase;
static void* concurrent_actor(void* raw) {
    ThreadCase* t=(ThreadCase*)raw;
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(
        t->id+100,1,8192,8,t->tenant,t->process);
    if(!a)return NULL;
    OresElasticNativeHandleV1 root,tail;
    if(ores_elastic_native_allocate_v1(a,4096,8,&root)) {
        const int64_t value=(int64_t)t->id+1100;
        assert(ores_elastic_native_write_i64_v1(a,root,0,value));
        /* A relocation may fail under contention; it must not corrupt root. */
        if(ores_elastic_native_allocate_v1(a,4096,8,&tail)) {
            int64_t z=-1;
            assert(ores_elastic_native_read_i64_v1(a,tail,0,&z) && z==0);
        }
        int64_t observed=0;
        assert(ores_elastic_native_read_i64_v1(a,root,0,&observed));
        assert(observed==value);
        assert(ores_elastic_budget_used_v1(t->tenant)<=32*KiB);
        assert(ores_elastic_budget_used_v1(t->process)<=32*KiB);
        t->success=1;
    }
    ores_elastic_native_close_v1(a);
    return NULL;
}
static void test_concurrent_actor_budget_admission(void) {
    OresElasticNativeBudgetV1 tenant,process;
    assert(ores_elastic_budget_init_v1(&tenant,32*KiB));
    assert(ores_elastic_budget_init_v1(&process,32*KiB));
    pthread_t threads[16];ThreadCase cases[16];
    for(unsigned i=0;i<16;++i){
        cases[i]=(ThreadCase){&tenant,&process,i,0};
        assert(pthread_create(&threads[i],NULL,concurrent_actor,&cases[i])==0);
    }
    for(unsigned i=0;i<16;++i)assert(pthread_join(threads[i],NULL)==0);
    int succeeded=0;
    for(unsigned i=0;i<16;++i)succeeded+=cases[i].success;
    assert(succeeded>0);
    assert(!ores_elastic_budget_used_v1(&tenant));
    assert(!ores_elastic_budget_used_v1(&process));
}
static void test_persistent_root_churn_reuses_scrubbed_holes(void) {
    OresElasticNativeBudgetV1 budget;
    assert(ores_elastic_budget_init_v1(&budget,4096));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(
        300,1,256,3,&budget,&budget);
    assert(a);
    OresElasticNativeHandleV1 root,tail,old_tail;
    assert(ores_elastic_native_allocate_v1(a,8,8,&root));
    assert(ores_elastic_native_write_i64_v1(a,root,0,0x10203040ll));
    assert(ores_elastic_native_allocate_v1(a,248,8,&tail));
    assert(tail.word==2 && ores_elastic_native_committed_v1(a)==256);
    old_tail=tail;
    assert(ores_elastic_native_drop_v1(a,tail));
    int64_t value=0;
    for(int i=0;i<10000;++i) {
        /* The live root pins the cursor at the hard quota. Only a genuine
         * dead-hole allocation can keep this 10,000-turn workload running. */
        assert(ores_elastic_native_allocate_v1(a,248,8,&tail));
        assert(tail.word==2 && tail.slot==old_tail.slot);
        assert(ores_elastic_native_read_i64_v1(a,tail,0,&value) && value==0);
        assert(ores_elastic_native_write_i64_v1(a,tail,0,i+100));
        assert(ores_elastic_native_read_i64_v1(a,root,0,&value) && value==0x10203040ll);
        assert(!ores_elastic_native_read_i64_v1(a,old_tail,0,&value));
        assert(ores_elastic_native_drop_v1(a,tail));
        old_tail=tail;
    }
    assert(ores_elastic_native_committed_v1(a)==256);
    assert(ores_elastic_budget_used_v1(&budget)==256);
    assert(ores_elastic_native_drop_v1(a,root));
    /* Last-owner drop resets the cursor and invalidates all cached holes:
     * ancient hole metadata cannot alias newly appended live objects. */
    OresElasticNativeHandleV1 fresh,aligned;
    assert(ores_elastic_native_allocate_v1(a,8,8,&fresh));
    assert(fresh.word==1 && fresh.generation!=root.generation);
    assert(ores_elastic_native_allocate_v1(a,64,64,&aligned));
    assert(aligned.word==9 && ores_elastic_native_live_v1(a)==2);
    assert(!ores_elastic_native_read_i64_v1(a,old_tail,0,&value));
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&budget)==0);
}
static void test_hole_alignment_and_nonoverlap(void) {
    OresElasticNativeBudgetV1 budget;
    assert(ores_elastic_budget_init_v1(&budget,4096));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(
        301,1,384,4,&budget,&budget);
    assert(a);
    OresElasticNativeHandleV1 root,aligned,reused,other;
    assert(ores_elastic_native_allocate_v1(a,8,8,&root));
    assert(ores_elastic_native_allocate_v1(a,64,64,&aligned));
    assert(aligned.word==9);
    assert(ores_elastic_native_drop_v1(a,aligned));
    assert(ores_elastic_native_allocate_v1(a,16,64,&reused));
    assert(reused.word==9 && reused.slot==aligned.slot);
    assert(ores_elastic_native_allocate_v1(a,64,8,&other));
    assert(other.word==17);
    assert(ores_elastic_native_write_i64_v1(a,reused,0,81));
    assert(ores_elastic_native_write_i64_v1(a,other,0,92));
    assert(ores_elastic_native_drop_v1(a,reused));
    int64_t v=0;
    assert(ores_elastic_native_read_i64_v1(a,other,0,&v) && v==92);
    assert(ores_elastic_native_drop_v1(a,other));
    assert(ores_elastic_native_drop_v1(a,root));
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&budget)==0);
}
static void test_forged_ref32_stride_cannot_wrap_32bit_host_size_t(void) {
    OresElasticNativeBudgetV1 budget;
    assert(ores_elastic_budget_init_v1(&budget,4096));
    OresElasticNativeArenaV1* a=ores_elastic_native_open_v1(
        500,1,1024,4,&budget,&budget);
    assert(a);
    OresElasticNativeHandleV1 authentic;
    assert(ores_elastic_native_allocate_v1(a,8,8,&authentic));
    assert(authentic.word==1);
    assert(ores_elastic_native_write_i64_v1(a,authentic,0,431));
    /* (0x20000001-1)*8 wraps to zero if decoded in 32-bit size_t. */
    OresElasticNativeHandleV1 forged=authentic;
    forged.word=UINT32_C(0x20000001);
    int64_t observed=0;
    assert(!ores_elastic_native_read_i64_v1(a,forged,0,&observed));
    assert(!ores_elastic_native_write_i64_v1(a,forged,0,777));
    assert(!ores_elastic_native_drop_v1(a,forged));
    assert(ores_elastic_native_read_i64_v1(a,authentic,0,&observed) && observed==431);
    ores_elastic_native_close_v1(a);
    assert(ores_elastic_budget_used_v1(&budget)==0);
}
int main(void){
    test_forged_ref32_stride_cannot_wrap_32bit_host_size_t();
    test_peak_quota_and_actor_survival();
    test_relocation_and_injected_failure();
    test_peak_limit_rejects_relocation();
    test_stale_slot_offset_collision_denied();
    test_concurrent_actor_budget_admission();
    test_persistent_root_churn_reuses_scrubbed_holes();
    test_hole_alignment_and_nonoverlap();
    puts("PASS host-only LLVM elastic native arena v1: peak budgets, rollback, ref32, sibling isolation");
    return 0;
}
