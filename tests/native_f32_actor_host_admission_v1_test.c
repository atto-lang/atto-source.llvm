/* HOST-ONLY: exact current NativeExecutionAuthority+ExecutionContext C structs.
 * No AttoLang guest or OresLLM private production kernel is executed.
 */
#include "oreslang/runtime/NativeF32ActorHostAdmissionV1.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(e) do { if (!(e)) { fprintf(stderr,"F32_HOST_GATE_FAIL:%d:%s\n",__LINE__,#e); abort(); } } while(0)
#define EXPECT(code,expr) REQUIRE((expr)==(code))
typedef struct {
    const OresF32ActorHostScope *scope;
    OresF32HostAdmissionResult result;
} ForeignAttempt;
static void *foreign_carrier(void *arg) {
    ForeignAttempt *a=(ForeignAttempt *)arg;
    a->result=ores_f32_host_preflight_v1(a->scope,2,3,3,2,4096);
    return NULL;
}
static void test_running_scope_and_forgery(void) {
    OresNativeExecutionAuthority authority;
    REQUIRE(ores_execution_init(&authority));
    OresExecutionClaim claim={0};
    REQUIRE(ores_execution_enter(&authority,1,17,0,&claim));
    OresNativeExecutionContext ctx={
        {1,42},{ORES_ALLOCATION_DOMAIN_ACTOR,11,77,9},{0,0},{0,0}
    };
    OresF32ActorHostScope scope={&ctx,&authority,claim,1,42,11,77,9};
    EXPECT(ORES_F32_HOST_PREFLIGHT_ONLY,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    EXPECT(ORES_F32_HOST_INVALID,ores_f32_host_preflight_v1(NULL,2,3,3,2,4096));
    EXPECT(ORES_F32_HOST_BAD_SHAPE,ores_f32_host_preflight_v1(&scope,0,3,3,2,4096));
    EXPECT(ORES_F32_HOST_BAD_SHAPE,ores_f32_host_preflight_v1(&scope,33,3,3,2,4096));
    EXPECT(ORES_F32_HOST_BAD_SHAPE,ores_f32_host_preflight_v1(&scope,2,3,2,2,4096));
    EXPECT(ORES_F32_HOST_BAD_SHAPE,ores_f32_host_preflight_v1(&scope,2,3,3,2,4097));
    EXPECT(ORES_F32_HOST_QUOTA,ores_f32_host_preflight_v1(&scope,2,3,3,2,15));
    EXPECT(ORES_F32_HOST_PREFLIGHT_ONLY,ores_f32_host_preflight_v1(&scope,32,32,32,32,4096));
    EXPECT(ORES_F32_HOST_QUOTA,ores_f32_host_preflight_v1(&scope,32,32,32,32,4095));

    scope.expected_actor_owner=88;
    EXPECT(ORES_F32_HOST_WRONG_ACTOR,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    scope.expected_actor_owner=77;
    scope.expected_actor_generation=10;
    EXPECT(ORES_F32_HOST_WRONG_ACTOR,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    scope.expected_actor_generation=9;
    ctx.allocation.kind=ORES_ALLOCATION_DOMAIN_TASK;
    EXPECT(ORES_F32_HOST_WRONG_ACTOR,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    ctx.allocation.kind=ORES_ALLOCATION_DOMAIN_ACTOR;
    ctx.id.execution=43;
    EXPECT(ORES_F32_HOST_WRONG_ACTOR,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    ctx.id.execution=42;

    ForeignAttempt stolen={&scope,ORES_F32_HOST_INVALID};
    pthread_t thief;
    REQUIRE(pthread_create(&thief,NULL,foreign_carrier,&stolen)==0);
    REQUIRE(pthread_join(thief,NULL)==0);
    EXPECT(ORES_F32_HOST_NOT_RUNNING,stolen.result);
    EXPECT(ORES_F32_HOST_PREFLIGHT_ONLY,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    REQUIRE(ores_execution_stop(&authority,1));
    EXPECT(ORES_F32_HOST_NOT_RUNNING,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    REQUIRE(ores_execution_leave(&authority,claim,0));
    REQUIRE(ores_execution_retired(&authority,1));
    REQUIRE(ores_execution_restart(&authority)==2);
    EXPECT(ORES_F32_HOST_NOT_RUNNING,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    OresExecutionClaim next={0};
    REQUIRE(ores_execution_enter(&authority,2,18,0,&next));
    scope.physical_claim=next;
    EXPECT(ORES_F32_HOST_PREFLIGHT_ONLY,ores_f32_host_preflight_v1(&scope,2,3,3,2,4096));
    REQUIRE(ores_execution_leave(&authority,next,0));
    REQUIRE(ores_execution_stop(&authority,2));
    REQUIRE(ores_execution_retired(&authority,2));
    REQUIRE(ores_execution_destroy(&authority));
}
int main(void) {
    test_running_scope_and_forgery();
    puts("ORES_F32_NATIVE_HOST_PREFLIGHT_V1_PASS|foreign_thread=DENIED|stopped=DENIED|shape=CHECKED|guest=NOT_EXECUTED");
    return 0;
}
