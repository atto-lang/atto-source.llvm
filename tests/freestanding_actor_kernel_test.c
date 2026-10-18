#include "oreslang/runtime/FreestandingActorKernel.h"
#include <assert.h>
#include <stdio.h>

static OresKernel *active_kernel;
static OresKernelRef active_ref;
static int64_t fault_payload = INT64_C(-42);

static int accumulate(uint64_t fields[ORES_KERNEL_FIELDS],int64_t payload,int64_t *result){
    /* Test values fit in int64_t. No signed overflow in this fixture. */
    fields[0] += (uint64_t)payload;
    *result = (int64_t)fields[0];
    return 1;
}
static int fault(uint64_t fields[ORES_KERNEL_FIELDS],int64_t payload,int64_t *result){
    if (payload == fault_payload) return 0;
    return accumulate(fields,payload,result);
}
static int reenter(uint64_t fields[ORES_KERNEL_FIELDS],int64_t payload,int64_t *result){
    assert(ores_kernel_send(active_kernel,active_ref,1) == ORES_KERNEL_BUSY);
    assert(ores_kernel_stop(active_kernel,active_ref) == ORES_KERNEL_BUSY);
    assert(ores_kernel_tick(active_kernel) == ORES_KERNEL_REENTRANT);
    return accumulate(fields,payload,result);
}
int main(void){
    OresKernel vm = {0};
    OresKernelRef a,b,c;
    uint64_t done;
    int64_t last;
    assert(ores_kernel_init(&vm)==ORES_KERNEL_OK);
    assert(ores_kernel_spawn(&vm,accumulate,&a)==ORES_KERNEL_OK);
    assert(ores_kernel_spawn(&vm,accumulate,&b)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,a,1)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,b,10)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,a,2)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,b,20)==ORES_KERNEL_OK);
    assert(ores_kernel_stop(&vm,a)==ORES_KERNEL_OK);
    assert(ores_kernel_stop(&vm,b)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,a,3)==ORES_KERNEL_CLOSED);
    assert(ores_kernel_stop(&vm,a)==ORES_KERNEL_CLOSED);
    for(unsigned i=0;i<4;i++) assert(ores_kernel_tick(&vm)==ORES_KERNEL_DELIVERED);
    assert(ores_kernel_snapshot(&vm,a,&done,&last)==ORES_KERNEL_OK);
    assert(done==2 && last==3);
    assert(ores_kernel_snapshot(&vm,b,&done,&last)==ORES_KERNEL_OK);
    assert(done==2 && last==30);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_IDLE);
    assert(ores_kernel_snapshot(&vm,a,&done,&last)==ORES_KERNEL_INVALID);
    assert(ores_kernel_send(&vm,a,1)==ORES_KERNEL_INVALID);
    assert(ores_kernel_stop(&vm,a)==ORES_KERNEL_INVALID);

    /* Reuse invalidates old capability generation; even if slot is reused. */
    for(unsigned n=0;n<ORES_KERNEL_ACTORS-2;n++){
        OresKernelRef r;
        assert(ores_kernel_spawn(&vm,accumulate,&r)==ORES_KERNEL_OK);
        assert(ores_kernel_stop(&vm,r)==ORES_KERNEL_OK);
        assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);
    }
    assert(ores_kernel_spawn(&vm,accumulate,&c)==ORES_KERNEL_OK);
    assert(c.slot==a.slot && c.generation==a.generation+1);
    assert(ores_kernel_send(&vm,a,10)==ORES_KERNEL_INVALID);
    for(unsigned n=0;n<ORES_KERNEL_MAILBOX;n++) assert(ores_kernel_send(&vm,c,1)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,c,99)==ORES_KERNEL_FULL);
    assert(ores_kernel_stop(&vm,c)==ORES_KERNEL_OK);
    for(unsigned n=0;n<ORES_KERNEL_MAILBOX;n++) assert(ores_kernel_tick(&vm)==ORES_KERNEL_DELIVERED);
    assert(ores_kernel_snapshot(&vm,c,&done,&last)==ORES_KERNEL_OK);
    assert(done==ORES_KERNEL_MAILBOX && last==ORES_KERNEL_MAILBOX);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);

    /* Handler fault retires only that actor, clears all owned data. */
    assert(ores_kernel_spawn(&vm,fault,&a)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,a,fault_payload)==ORES_KERNEL_OK);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_HANDLER_FAILED);
    assert(vm.fault_count==1);
    assert(ores_kernel_send(&vm,a,0)==ORES_KERNEL_INVALID);

    assert(ores_kernel_spawn(&vm,reenter,&active_ref)==ORES_KERNEL_OK);
    active_kernel=&vm;
    assert(ores_kernel_send(&vm,active_ref,7)==ORES_KERNEL_OK);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_DELIVERED);
    assert(ores_kernel_stop(&vm,active_ref)==ORES_KERNEL_OK);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_IDLE);
    /* Reset must invalidate even a ref that was previously retired. */
    const OresKernelRef stale = active_ref;
    assert(ores_kernel_init(&vm)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,stale,7)==ORES_KERNEL_INVALID);
    assert(ores_kernel_spawn(&vm,accumulate,&a)==ORES_KERNEL_OK);
    assert(ores_kernel_send(&vm,stale,7)==ORES_KERNEL_INVALID);
    assert(ores_kernel_stop(&vm,a)==ORES_KERNEL_OK);
    assert(ores_kernel_tick(&vm)==ORES_KERNEL_RETIRED);
    puts("FREESTANDING_ACTOR_KERNEL_HOST_TEST_PASS");
    return 0;
}
