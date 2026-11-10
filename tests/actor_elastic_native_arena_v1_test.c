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
int main(void){
    test_peak_quota_and_actor_survival();
    test_relocation_and_injected_failure();
    test_peak_limit_rejects_relocation();
    test_stale_slot_offset_collision_denied();
    test_concurrent_actor_budget_admission();
    puts("PASS host-only LLVM elastic native arena v1: peak budgets, rollback, ref32, sibling isolation");
    return 0;
}
