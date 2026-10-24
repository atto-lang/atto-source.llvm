#ifdef NDEBUG
#undef NDEBUG
#endif
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdint.h>

static unsigned sender_messages;
static unsigned target_messages;
static int64_t target_last;

static void init_zero(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap,domain,epoch,0,0);
}

static int64_t target_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    target_messages++;
    target_last=next;
    return next;
}

static int64_t sender_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    sender_messages++;
    /* Capability slot 0 is runtime-owned. The source handler never receives
     * a target pointer or casts an ActorRef into scalar state. */
    assert(oreslang_source_actor_try_send_capability(heap,0,payload+1)==1);
    return next;
}

int main(void) {
    void* target=oreslang_source_actor_spawn(init_zero,target_handler,1);
    assert(target!=0);

    void* supplied[1]={target};
    void* sender=oreslang_source_actor_spawn_with_capabilities(
        init_zero,sender_handler,1,supplied,1);
    assert(sender!=0);

    /* The runtime must copy the generation-fenced capability at spawn time,
     * not retain the caller-owned array. */
    supplied[0]=0;

    assert(oreslang_source_actor_try_send(sender,4)==1);
    oreslang_source_actor_stop(sender);
    assert(sender_messages==1);

    oreslang_source_actor_stop(target);
    assert(target_messages==1);
    assert(target_last==5);
    return 0;
}
