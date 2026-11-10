#include "oreslang/runtime/ActorElasticNativeArenaV1.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
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
int main(void){
    test_peak_quota_and_actor_survival();
    test_relocation_and_injected_failure();
    test_peak_limit_rejects_relocation();
    puts("PASS host-only LLVM elastic native arena v1: peak budgets, rollback, ref32, sibling isolation");
    return 0;
}
