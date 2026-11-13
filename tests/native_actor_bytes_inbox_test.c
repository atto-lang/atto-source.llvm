#include "oreslang/runtime/NativeActorBytesInbox.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define check(condition) do { if (!(condition)) { fprintf(stderr, "FAIL at %d: %s\n", __LINE__, #condition); abort(); } ++assertions; } while (0)
static int equal(OresNativeActorBytesRef a, OresNativeActorBytesRef b) {
    return a.actor_id==b.actor_id && a.incarnation==b.incarnation && a.nonce==b.nonce;
}
static void receive(OresNativeActorBytesInbox* b, OresNativeActorBytesRef r,
                    const unsigned char* expected, size_t n) {
    unsigned char out[64]; memset(out, 0xb9, sizeof(out));
    size_t got = SIZE_MAX;
    check(ores_native_actor_bytes_try_receive(b, r, out, n, &got)==ORES_ACTOR_BYTES_OK);
    check(got==n);
    check(memcmp(out, expected, n)==0);
}
int main(void) {
    /* All failure cases are attempted BEFORE any allocation. */
    check(!ores_native_actor_bytes_open(0,1,2,64,128));
    check(!ores_native_actor_bytes_open(1,0,2,64,128));
    check(!ores_native_actor_bytes_open(1,1,0,64,128));
    check(!ores_native_actor_bytes_open(1,1,129,64,128));
    check(!ores_native_actor_bytes_open(1,1,2,65537,128));
    check(!ores_native_actor_bytes_open(1,1,2,64,129));
    check(!ores_native_actor_bytes_open(1,1,128,65536,8388609));
    OresNativeActorBytesInbox* a=ores_native_actor_bytes_open(17,42,3,64,10);
    OresNativeActorBytesInbox* b=ores_native_actor_bytes_open(18,42,2,64,64);
    check(a && b);
    OresNativeActorBytesRef ref=ores_native_actor_bytes_ref(a);
    OresNativeActorBytesRef other=ores_native_actor_bytes_ref(b);
    check(ref.nonce != other.nonce && !equal(ref,other));
    OresNativeActorBytesRef stale=ref; stale.incarnation++;
    check(ores_native_actor_bytes_try_send(a,stale,"x",1)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_send(a,other,"x",1)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_send(b,ref,"x",1)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_send(a,ref,NULL,1)==ORES_ACTOR_BYTES_DENIED);
    unsigned char bytes[] = {0,0xff,0x0a,0x0d,0x80,9,0,42};
    check(ores_native_actor_bytes_try_send(a,ref,bytes,8)==ORES_ACTOR_BYTES_OK);
    bytes[0] = 99; /* enqueue made an owned copy */
    check(ores_native_actor_bytes_try_send(a,ref,bytes,3)==ORES_ACTOR_BYTES_DENIED);
    size_t count=0,used=0;
    check(ores_native_actor_bytes_observe(a,ref,&count,&used)==ORES_ACTOR_BYTES_OK);
    check(count==1 && used==8);
    size_t actual=SIZE_MAX;
    unsigned char out[64]; memset(out,0xbb,sizeof(out));
    check(ores_native_actor_bytes_try_receive(a,stale,out,64,&actual)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_receive(a,ref,out,7,&actual)==ORES_ACTOR_BYTES_SMALL);
    check(actual==8 && out[0]==0xbb);
    check(ores_native_actor_bytes_try_receive(a,ref,out,64,&actual)==ORES_ACTOR_BYTES_OK);
    check(actual==8 && out[0]==0 && out[1]==0xff && out[4]==0x80);
    check(ores_native_actor_bytes_observe(a,ref,&count,&used)==ORES_ACTOR_BYTES_OK);
    check(count==0 && used==0);
    check(ores_native_actor_bytes_all_zero_for_test(a));
    unsigned char x[]={3,1,4,1,5},y[]={9,2,6},z[]={5,3};
    check(ores_native_actor_bytes_try_send(a,ref,x,sizeof(x))==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_send(a,ref,y,sizeof(y))==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_send(a,ref,z,sizeof(z))==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_send(a,ref,z,sizeof(z))==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_observe(a,ref,&count,&used)==ORES_ACTOR_BYTES_OK);
    check(count==3 && used==10);
    receive(a,ref,x,sizeof(x));
    check(ores_native_actor_bytes_try_send(a,ref,x,sizeof(x))==ORES_ACTOR_BYTES_OK);
    receive(a,ref,y,sizeof(y));
    receive(a,ref,z,sizeof(z));
    receive(a,ref,x,sizeof(x));
    check(ores_native_actor_bytes_try_receive(a,ref,out,sizeof(out),&actual)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_all_zero_for_test(a));
    /* 0-byte payload is a valid message and cannot consume byte credit. */
    check(ores_native_actor_bytes_try_send(b,other,NULL,0)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_receive(b,other,NULL,0,&actual)==ORES_ACTOR_BYTES_OK);
    check(actual==0);
    /* Retiring one inbox cannot mutate or retire an unrelated actor. */
    check(ores_native_actor_bytes_try_send(b,other,x,sizeof(x))==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_send(a,ref,x,sizeof(x))==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(a,ref)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_retire(a,other)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_retire(a,ref)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_all_zero_for_test(a));
    check(ores_native_actor_bytes_try_retire(a,ref)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_send(a,ref,x,sizeof(x))==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_receive(a,ref,out,sizeof(out),&actual)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_observe(a,ref,&count,&used)==ORES_ACTOR_BYTES_OK);
    check(count==0 && used==0);
    receive(b,other,x,sizeof(x));
    check(ores_native_actor_bytes_try_destroy(a,ref)==ORES_ACTOR_BYTES_OK);
    /* Reused IDs must never accept a stale incarnation/nonce. */
    OresNativeActorBytesInbox* recycled=ores_native_actor_bytes_open(17,42,1,64,64);
    check(recycled != NULL);
    OresNativeActorBytesRef new_ref=ores_native_actor_bytes_ref(recycled);
    check(!equal(ref,new_ref));
    check(ores_native_actor_bytes_try_send(recycled,ref,x,sizeof(x))==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_try_send(recycled,new_ref,x,sizeof(x))==ORES_ACTOR_BYTES_OK);
    receive(recycled,new_ref,x,sizeof(x));
    check(ores_native_actor_bytes_try_retire(recycled,new_ref)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(recycled,new_ref)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_retire(b,other)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(b,other)==ORES_ACTOR_BYTES_OK);
    /* Maximum physical budget, binary fidelity and full 128-slot pressure. */
    OresNativeActorBytesInbox* large =
        ores_native_actor_bytes_open(900, 6, 128, 65536, 128u * 65536u);
    check(large != NULL);
    OresNativeActorBytesRef lref = ores_native_actor_bytes_ref(large);
    unsigned char* full = (unsigned char*)malloc(65536);
    unsigned char* copied = (unsigned char*)malloc(65536);
    check(full && copied);
    for (size_t i = 0; i < 65536; ++i) full[i] = (unsigned char)(i * 31u);
    for (size_t i = 0; i < 128; ++i)
        check(ores_native_actor_bytes_try_send(
            large, lref, full, 65536)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_send(
        large, lref, full, 65536)==ORES_ACTOR_BYTES_DENIED);
    check(ores_native_actor_bytes_observe(
        large, lref, &count, &used)==ORES_ACTOR_BYTES_OK);
    check(count==128 && used==128u * 65536u);
    for (size_t i = 0; i < 128; ++i) {
        actual=0;
        check(ores_native_actor_bytes_try_receive(
            large, lref, copied, 65536, &actual)==ORES_ACTOR_BYTES_OK);
        check(actual==65536 && memcmp(copied,full,65536)==0);
    }
    check(ores_native_actor_bytes_all_zero_for_test(large));
    free(full); free(copied);
    check(ores_native_actor_bytes_try_retire(large,lref)==ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(large,lref)==ORES_ACTOR_BYTES_OK);
    printf("NATIVE_ACTOR_BYTES_INBOX_HOST_ONLY_PASS assertions=%u actor_private_slabs=2 copied_bytes=1 quota=1 fifo=1 incarnation=1 zeroized=1 nonparking=1\n",assertions);
    return 0;
}
