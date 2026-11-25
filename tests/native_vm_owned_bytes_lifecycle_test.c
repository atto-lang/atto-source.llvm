#define _POSIX_C_SOURCE 200809L
/* Actual four-domain OresVM/scheduler and VM-owned native Bytes slabs.
 * Host-only proof; no compiler-generated .atto guest or blocking IO path. */
#include "oreslang/runtime/NativeOresVMBytesOwned.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeVM* vm;
    OresNativeActorRef actors[2];
    atomic_int entered;
    atomic_int release;
    atomic_int primary_done;
    atomic_int peer_done;
    int hold_primary;
    uint64_t first_payload;
} Context;
static void check(int okay,const char* why) {
    if (!okay) { fprintf(stderr,"VM_OWNED_BYTES_FAIL %s\n",why); abort(); }
}
static void on_data(void* opaque,size_t actor_index,uint64_t payload) {
    Context* ctx=(Context*)opaque;
    check(actor_index<2,"actor index");
    unsigned char bytes[8]={0};size_t n=SIZE_MAX;
    const int result=ores_native_vm_bytes_receive_current(
        ctx->vm,ORES_NATIVE_VM_MAIN,ctx->actors[actor_index],
        bytes,sizeof(bytes),&n);
    check(result==ORES_ACTOR_BYTES_OK && n==4 &&
          bytes[1]==0 && bytes[2]==255 && bytes[3]==10,
          "physical current actor Bytes receipt");
    if (actor_index==0) {
        check(payload==ctx->first_payload && bytes[0]==10,
              "primary payload identity");
        atomic_fetch_add_explicit(&ctx->primary_done,1,memory_order_release);
        atomic_store_explicit(&ctx->entered,1,memory_order_release);
        if (ctx->hold_primary) {
            /* HOST TEST FIXTURE only: production actor DATA callbacks must
             * never spin, park or wait on I/O. */
            while (!atomic_load_explicit(&ctx->release,memory_order_acquire))
                sched_yield();
        }
    } else {
        const int before=atomic_fetch_add_explicit(
            &ctx->peer_done,1,memory_order_release);
        check((payload==2000u+(uint64_t)before) &&
              bytes[0]==(unsigned char)(20+before),"peer independent data");
    }
}
static OresNativeVM* open_vm(Context* ctx) {
    OresNativeVMConfig cfg={0};
    for (int i=0;i<ORES_NATIVE_VM_DOMAIN_COUNT;++i) {
        cfg.domains[i].actor_slots=2;
        cfg.domains[i].mailbox_capacity=4;
        cfg.domains[i].carrier_workers=2;
        cfg.domains[i].on_message=on_data;
        cfg.domains[i].context=ctx;
    }
    return ores_native_vm_open(&cfg);
}
static void wait_count(atomic_int* count,int expected,const char* why) {
    struct timespec nap={0,1000000};
    for(int i=0;i<5000;++i) {
        if (atomic_load_explicit(count,memory_order_acquire)==expected) return;
        nanosleep(&nap,NULL); /* trusted external host test thread only */
    }
    check(0,why);
}
int main(void) {
    Context ctx={0};ctx.hold_primary=1;ctx.first_payload=1000;
    ctx.vm=open_vm(&ctx);check(ctx.vm!=NULL,"open four-domain VM");
    for (size_t i=0;i<2;++i) {
        ctx.actors[i]=ores_native_vm_spawn(ctx.vm,ORES_NATIVE_VM_MAIN,i);
        check(ctx.actors[i].scheduler_domain&&ctx.actors[i].generation,
              "VM actor identity");
    }
    OresNativeActorRef stale=ctx.actors[0];stale.generation++;
    check(!ores_native_vm_bytes_enable(
        ctx.vm,ORES_NATIVE_VM_MAIN,stale,4,16,64),
        "stale-generation install refused");
    check(!ores_native_vm_bytes_enable(
        ctx.vm,ORES_NATIVE_VM_SHARED,ctx.actors[0],4,16,64),
        "foreign-domain install refused");
    for(size_t i=0;i<2;++i)
        check(ores_native_vm_bytes_enable(
            ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[i],4,16,64),
            "VM owns private inbox");
    check(!ores_native_vm_bytes_enable(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],4,16,64),
        "duplicate owned resource denied");
    unsigned char received[8]={0};size_t length=SIZE_MAX;
    check(ores_native_vm_bytes_receive_current(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],
        received,sizeof(received),&length)==ORES_ACTOR_BYTES_DENIED
        && length==SIZE_MAX,"external caller never receives guest bytes");
    unsigned char first[4]={10,0,255,10}, second[4]={11,0,255,10};
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,stale,1999,first,4)
        ==ORES_NATIVE_SEND_INVALID_REF,"stale send denied");
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],1000,first,4)
        ==ORES_NATIVE_SEND_ACCEPTED,"primary VM atomic DATA+Bytes");
    wait_count(&ctx.entered,1,"primary active actor turn");
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],1001,second,4)
        ==ORES_NATIVE_SEND_ACCEPTED,"second queued before sealed STOP");
    size_t pending=0,used=0;
    check(ores_native_vm_bytes_observe(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],&pending,&used)
        && pending==1&&used==4,"pre-STOP private byte charge");
    unsigned char peer0[4]={20,0,255,10};
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[1],2000,peer0,4)
        ==ORES_NATIVE_SEND_ACCEPTED,"peer can run concurrently");
    wait_count(&ctx.peer_done,1,"peer processed independently");
    check(ores_native_vm_stop_sealed(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0]),"sealed actor STOP");
    check(!ores_native_vm_poll_done(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0]),
        "cannot reclaim during running physical actor turn");
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],1998,second,4)
        ==ORES_NATIVE_SEND_TERMINATED,
        "STOP closes VM Bytes admission before memory retire");
    check(ores_native_vm_bytes_observe(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],&pending,&used)
        && pending==1&&used==4,
        "abandoned bytes retained until DONE and automatic cleanup");
    atomic_store_explicit(&ctx.release,1,memory_order_release);
    check(ores_native_vm_wait_done(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0]),
        "VM wait_done automatically scrubbed/freed private inbox");
    OresNativeActorTerminalState terminal={0};
    check(ores_native_vm_actor_terminal_state(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],&terminal)
        && terminal.accepted_user_messages==2
        && terminal.processed_user_messages==1
        && terminal.abandoned_user_messages==1,
        "exact terminal processed+abandoned accounting");
    check(!ores_native_vm_bytes_observe(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],&pending,&used),
        "VM resource released automatically on DONE");
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[0],1002,first,4)
        ==ORES_NATIVE_SEND_INVALID_REF,
        "post-finalization cannot reanimate freed inbox");
    unsigned char peer1[4]={21,0,255,10};
    check(ores_native_vm_bytes_try_send(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[1],2001,peer1,4)
        ==ORES_NATIVE_SEND_ACCEPTED,"peer retains independent arena");
    wait_count(&ctx.peer_done,2,"peer second receive");
    check(ores_native_vm_stop(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[1]),"peer STOP");
    check(ores_native_vm_wait_done(
        ctx.vm,ORES_NATIVE_VM_MAIN,ctx.actors[1]),"peer auto-finalized");
    check(ores_native_vm_close(ctx.vm),"VM closed after auto cleanup");

    /* A second VM instance must sweep owned bytes automatically on close,
     * even when the host never calls wait_done/poll_done/try_retire. */
    Context close_only={0};close_only.first_payload=3000;
    close_only.vm=open_vm(&close_only);
    check(close_only.vm!=NULL,"second VM");
    close_only.actors[0]=ores_native_vm_spawn(
        close_only.vm,ORES_NATIVE_VM_MAIN,0);
    check(close_only.actors[0].scheduler_domain!=0,"second incarnation");
    check(ores_native_vm_bytes_enable(
        close_only.vm,ORES_NATIVE_VM_MAIN,close_only.actors[0],4,16,64),
        "second private slab");
    check(ores_native_vm_bytes_try_send(
        close_only.vm,ORES_NATIVE_VM_MAIN,close_only.actors[0],
        3000,first,4)==ORES_NATIVE_SEND_ACCEPTED,"close-only message");
    wait_count(&close_only.primary_done,1,"close-only processed");
    check(ores_native_vm_close(close_only.vm),
        "VM close automatically drains and destroys owned slabs");
    puts("NATIVE_VM_OWNED_BYTES_LIFECYCLE_HOST_ONLY_PASS domains=4 active_turn_guard=1 stale=DENIED stopped=TERMINATED peer=ISOLATED wait_done_autoscrub=1 close_autoscrub=1");
    return 0;
}
