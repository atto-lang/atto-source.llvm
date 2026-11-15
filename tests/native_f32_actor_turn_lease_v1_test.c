/* Source-owned actor turn lifetime test: not real GEMM/compiler execution. */
#include "oreslang/runtime/NativeF32ActorTurnLeaseV1.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if(!(x)) {fprintf(stderr,"F32_LEASE_FAIL:%d:%s\n",__LINE__,#x);abort();} }while(0)

typedef struct {
  OresF32ActorTurnLeaseV1 *lease;
  OresF32ActorOperationPinV1 ticket;
  _Atomic int *stopped;
  _Atomic int *quiesced;
  int stolen_return;
} Attempt;
static void *foreign_return(void *ptr) {
  Attempt *a=ptr;
  a->stolen_return=ores_f32_actor_lease_leave_v1(a->ticket);
  return NULL;
}
static void *stop_and_drain(void *ptr) {
  Attempt *a=ptr;
  REQUIRE(ores_f32_actor_lease_stop_v1(a->lease,1));
  atomic_store(a->stopped,1);
  REQUIRE(ores_f32_actor_lease_wait_quiescent_v1(a->lease));
  atomic_store(a->quiesced,1);
  return NULL;
}
static void test_host_stop_race(void) {
  OresNativeExecutionAuthority authority;
  REQUIRE(ores_execution_init(&authority));
  OresExecutionClaim claim={0};
  REQUIRE(ores_execution_enter(&authority,1,17,0,&claim));
  OresNativeExecutionContext ctx={{1,42},{ORES_ALLOCATION_DOMAIN_ACTOR,11,77,9},{0,0},{0,0}};
  OresF32ActorHostScope scope={&ctx,&authority,claim,1,42,11,77,9};
  OresF32ActorTurnLeaseV1 lease;
  REQUIRE(ores_f32_actor_lease_init_v1(&lease,&authority));
  OresF32ActorOperationPinV1 ticket={0};
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_PREFLIGHT_ONLY);
  REQUIRE(!ores_f32_actor_lease_destroy_v1(&lease));
  REQUIRE(!ores_f32_actor_lease_wait_quiescent_v1(&lease)); /* no self-deadlock */
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_NOT_RUNNING);
  /* Rejected nested admission must not have overwritten the original ticket. */
  REQUIRE(ticket.serial!=0);
  _Atomic int stopped=0,quiesced=0;
  Attempt args={&lease,ticket,&stopped,&quiesced,-1};
  pthread_t other;
  REQUIRE(pthread_create(&other,NULL,foreign_return,&args)==0);
  REQUIRE(pthread_join(other,NULL)==0);
  REQUIRE(args.stolen_return==0);
  REQUIRE(pthread_create(&other,NULL,stop_and_drain,&args)==0);
  while(!atomic_load(&stopped))sched_yield();
  REQUIRE(!atomic_load(&quiesced));
  REQUIRE(ores_execution_phase(&authority)==ORES_EXEC_STOPPING);
  REQUIRE(!ores_execution_retired(&authority,1));
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_NOT_RUNNING);
  REQUIRE(ores_f32_actor_lease_leave_v1(ticket));
  REQUIRE(!ores_f32_actor_lease_leave_v1(ticket));
  REQUIRE(pthread_join(other,NULL)==0);
  REQUIRE(atomic_load(&quiesced)==1);
  REQUIRE(ores_execution_leave(&authority,claim,0));
  REQUIRE(ores_execution_retired(&authority,1));
  REQUIRE(ores_execution_restart(&authority)==2);
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_NOT_RUNNING);
  REQUIRE(ores_f32_actor_lease_destroy_v1(&lease));
  REQUIRE(ores_execution_destroy(&authority));
}
static void test_repeated_generation_and_bounds(void) {
  OresNativeExecutionAuthority authority;
  REQUIRE(ores_execution_init(&authority));
  OresExecutionClaim claim={0};
  REQUIRE(ores_execution_enter(&authority,1,71,0,&claim));
  OresNativeExecutionContext ctx={{1,22},{ORES_ALLOCATION_DOMAIN_ACTOR,12,6,4},{0,0},{0,0}};
  OresF32ActorHostScope scope={&ctx,&authority,claim,1,22,12,6,4};
  OresF32ActorTurnLeaseV1 lease;
  REQUIRE(ores_f32_actor_lease_init_v1(&lease,&authority));
  OresF32ActorOperationPinV1 ticket={0};
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,2,2,4096,&ticket)==ORES_F32_HOST_BAD_SHAPE);
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,32,32,32,32,4095,&ticket)==ORES_F32_HOST_QUOTA);
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,32,32,32,32,4096,&ticket)==ORES_F32_HOST_PREFLIGHT_ONLY);
  REQUIRE(ores_f32_actor_lease_leave_v1(ticket));
  OresF32ActorOperationPinV1 previous=ticket;
  for(unsigned i=0;i<1024;i++) {
    REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_PREFLIGHT_ONLY);
    REQUIRE(!ores_f32_actor_lease_leave_v1(previous));
    REQUIRE(ores_f32_actor_lease_leave_v1(ticket));
    previous=ticket;
  }
  scope.expected_actor_generation++;
  REQUIRE(ores_f32_actor_lease_enter_v1(&lease,&scope,2,3,3,2,4096,&ticket)==ORES_F32_HOST_WRONG_ACTOR);
  scope.expected_actor_generation--;
  REQUIRE(ores_f32_actor_lease_stop_v1(&lease,1));
  REQUIRE(ores_f32_actor_lease_wait_quiescent_v1(&lease));
  REQUIRE(ores_execution_leave(&authority,claim,0));
  REQUIRE(ores_execution_retired(&authority,1));
  REQUIRE(ores_f32_actor_lease_destroy_v1(&lease));
  REQUIRE(ores_execution_destroy(&authority));
}
int main(void){
  test_host_stop_race();test_repeated_generation_and_bounds();
  puts("ORES_F32_LLVM_PINNED_STOP_RACE_PASS|reuse=1024|foreign=DENIED|guest=NOT_EXECUTED");
}
