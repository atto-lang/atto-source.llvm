#ifdef NDEBUG
#undef NDEBUG
#endif
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

static atomic_uint sender_messages;
static atomic_uint target_messages;
static atomic_int_fast64_t target_last;
static atomic_uint nested_parent_messages;
static atomic_uint nested_child_messages;
static atomic_int_fast64_t nested_child_last;

static void init_zero(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap,domain,epoch,0,0);
}

static int64_t target_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    atomic_fetch_add_explicit(&target_messages,1,memory_order_relaxed);
    atomic_store_explicit(&target_last,next,memory_order_relaxed);
    return next;
}

static int64_t sender_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    atomic_fetch_add_explicit(&sender_messages,1,memory_order_relaxed);
    /* Capability slot 0 is runtime-owned. The source handler never receives
     * a target pointer or casts an ActorRef into scalar state. */
    assert(oreslang_source_actor_try_send_capability(heap,0,payload+1)==1);
    return next;
}

static int64_t nested_child_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next=prior+payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    atomic_fetch_add_explicit(&nested_child_messages,1,memory_order_relaxed);
    atomic_store_explicit(&nested_child_last,next,memory_order_relaxed);
    return next;
}

static void nested_parent_init(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap,domain,epoch,0,0);
    oreslang_source_actor_spawn_child_capability(
        heap,0,init_zero,nested_child_handler,1);
}

static int64_t nested_parent_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    const int64_t next=prior+payload;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,next);
    atomic_fetch_add_explicit(&nested_parent_messages,1,memory_order_relaxed);
    oreslang_source_actor_send_capability(heap,0,payload+1);
    return next;
}

static int64_t output_end_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)domain;
    (void)epoch;
    oreslang_source_actor_emit_output(heap,payload+1);
    oreslang_source_actor_request_end(heap);
    return 0;
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
    assert(atomic_load_explicit(&sender_messages,memory_order_relaxed)==1);

    oreslang_source_actor_stop(target);
    assert(atomic_load_explicit(&target_messages,memory_order_relaxed)==1);
    assert(atomic_load_explicit(&target_last,memory_order_relaxed)==5);

    /* Second session: the parent constructor owns a child capability that is
     * not exposed to the host. Stopping the parent must drain/retire the child
     * after the parent handler has enqueued its message. */
    void* parent=oreslang_source_actor_spawn(
        nested_parent_init,nested_parent_handler,1);
    assert(parent!=0);
    assert(oreslang_source_actor_try_send(parent,41)==1);
    oreslang_source_actor_stop(parent);
    assert(atomic_load_explicit(
        &nested_parent_messages,memory_order_relaxed)==1);
    assert(atomic_load_explicit(
        &nested_child_messages,memory_order_relaxed)==1);
    assert(atomic_load_explicit(
        &nested_child_last,memory_order_relaxed)==42);

    /* Third session: self.send is an output stream, not self-mailbox recursion,
     * and self.end is a nonblocking lifecycle request from the active turn. */
    void* output_actor=oreslang_source_actor_spawn(
        init_zero,output_end_handler,1);
    assert(output_actor!=0);
    assert(oreslang_source_actor_try_send(output_actor,41)==1);
    assert(oreslang_source_actor_take_output(output_actor)==42);
    oreslang_source_actor_stop(output_actor);
    return 0;
}
