// Real compiled native guest + existing native continuation transaction store.
// Host harness, not yet production TASK/WAKE transport.
#include "oreslang/runtime/NativeContinuationStore.h"
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
typedef int64_t I;
#define ARGS I*,I,I
extern I unwind_start_v1(ARGS),unwind_pending_v1(ARGS),unwind_resume_v1(ARGS),unwind_outcome_v1(ARGS),unwind_error_v1(ARGS),unwind_retire_v1(ARGS);
extern I unwind_complete_v1(ARGS,I,I,I),unwind_result_v1(ARGS,I*);
typedef struct {
 I slot,kind,value;
 int committed;
} Receipt;
typedef struct {
 OresNativeContinuationStore *store;
 OresNativeContinuationHandle ticket;
 Receipt *receipt;
 I expected,slot,kind,value;
 int won;
} Publication;
static int commit(void *raw){
 Publication *p=raw;
 if(p->receipt->committed||p->slot!=p->expected||p->kind<0||p->kind>2||(p->kind&&p->value<=0))return 0;
 *p->receipt=(Receipt){p->slot,p->kind,p->value,1};
 return 1;
}
static void *producer(void *raw){
 Publication *p=raw;
 p->won=ores_native_continuation_store_commit_ready(p->store,p->ticket,commit,p);
 return NULL;
}
static void publish(Publication *p){
 pthread_t t;assert(pthread_create(&t,NULL,producer,p)==0);assert(pthread_join(t,NULL)==0);
}
int main(void){
 OresNativeExecutionContextStore *xs=ores_native_execution_context_store_open(1);
 assert(xs);
 OresNativeContinuationStore *cs=ores_native_continuation_store_open(1,xs);
 assert(cs);
 OresNativeExecutionContext context={.id={88,7},.allocation={ORES_ALLOCATION_DOMAIN_ACTOR,88,7,1}};
 OresNativeExecutionHandle x={0};
 assert(ores_native_execution_context_store_insert(xs,&context,&x));
 // One explicit host lifetime pin spans all awaits and terminal publication.
 assert(ores_native_execution_context_store_retain(xs,x));
 assert(ores_native_execution_context_store_release(xs,x));
 I frame[26]={0};
 assert(unwind_start_v1(frame,7,1)==1);
 OresNativeContinuationHandle stale={0};
 for(I i=1;i<=3;i++){
  OresNativeContinuationHandle ticket={0};
  assert(unwind_pending_v1(frame,7,1)==i);
  assert(ores_native_continuation_store_publish(cs,x,(OresNativeContinuationHandle){0},ORES_CONTINUATION_BOUNDARY_AWAIT,1,(uint64_t)i,&ticket));
  if(i>1)assert(!ores_native_continuation_store_notify_ready(cs,stale));
  assert(!ores_native_continuation_store_try_claim_ready_resume(cs,ticket,NULL));
  assert(unwind_resume_v1(frame,7,1)==-1);
  Receipt cell={0};
  Publication p={cs,ticket,&cell,i,i+1,0,0,0};
  publish(&p);assert(!p.won&&!cell.committed);
  p.slot=i;p.kind=(i==1);p.value=i==1?11:i==2?0:42;
  publish(&p);assert(p.won&&cell.committed);
  p.value=999;publish(&p);assert(!p.won&&cell.value!=999);
  // Producer did not execute any native guest code.
  assert(unwind_pending_v1(frame,7,1)==i);
  assert(unwind_resume_v1(frame,7,1)==-1);
  assert(unwind_retire_v1(frame,7,1)==-1);
  OresNativeContinuationRecord r={0};
  assert(ores_native_continuation_store_try_claim_ready_resume(cs,ticket,&r));
  assert(r.execution_id.runtime_domain==88&&r.execution_id.execution==7);
  assert(r.allocation.owner==7&&r.allocation.generation==1&&r.source_token==(uint64_t)i);
  assert(!ores_native_continuation_store_try_claim_ready_resume(cs,ticket,NULL));
  assert(!ores_native_continuation_store_release(cs,ticket));
  assert(unwind_complete_v1(frame,7,1,cell.slot,cell.kind,cell.value)==0);
  assert(unwind_resume_v1(frame,7,1)==(i==3?2:1));
  assert(ores_native_continuation_store_finish_resume(cs,ticket));
  assert(ores_native_continuation_store_release(cs,ticket));
  assert(ores_native_continuation_store_live(cs)==0);
  assert(ores_native_execution_context_store_live(xs)==1);
  stale=ticket;
 }
 I result=0;
 assert(unwind_outcome_v1(frame,7,1)==2&&unwind_error_v1(frame,7,1)==11);
 assert(unwind_result_v1(frame,7,1,&result)==0&&result==42);
 assert(ores_native_execution_context_store_live(xs)==1);
 assert(unwind_retire_v1(frame,7,1)==0);
 assert(ores_native_execution_context_store_release(xs,x));
 assert(ores_native_execution_context_store_live(xs)==0);
 assert(ores_native_continuation_store_close(cs));
 assert(ores_native_execution_context_store_close(xs));
 puts("PASS_NATIVE_OWNED_UNWIND_STORE");return 0;
}
