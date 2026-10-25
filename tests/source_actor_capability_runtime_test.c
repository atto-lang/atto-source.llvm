#ifdef NDEBUG
#undef NDEBUG
#endif
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static atomic_uint sender_messages;
static atomic_uint target_messages;
static atomic_int_fast64_t target_last;
static atomic_uint nested_parent_messages;
static atomic_uint nested_child_messages;
static atomic_int_fast64_t nested_child_last;

static void wait_for_count(atomic_uint* value, unsigned expected) {
    for (unsigned i = 0; i < 1000000u; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
            return;
        sched_yield();
    }
    assert(!"timed out waiting for source actor capability delivery");
}

static void init_zero(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 0);
}

static int64_t target_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, next);
    atomic_store_explicit(&target_last, next, memory_order_relaxed);
    atomic_fetch_add_explicit(&target_messages, 1, memory_order_release);
    return next;
}

static int64_t sender_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, next);

    /* Slot 0 is a copied generation-fenced capability. No target pointer or
     * guest i64 representation is available to the source handler. */
    assert(oreslang_source_actor_try_send_capability(
        heap, 0, payload + 1) == 1);
    atomic_fetch_add_explicit(&sender_messages, 1, memory_order_release);
    return next;
}

static int64_t nested_child_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, next);
    atomic_store_explicit(&nested_child_last, next, memory_order_relaxed);
    atomic_fetch_add_explicit(&nested_child_messages, 1, memory_order_release);
    return next;
}

static void nested_parent_init(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 0);
    oreslang_source_actor_spawn_child_capability(
        heap, 0, init_zero, nested_child_handler, 1);
}

static int64_t nested_parent_handler(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    const int64_t prior =
        ores_actor_i64_heap_field_read(heap, domain, epoch, 0);
    const int64_t next = prior + payload;
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, next);
    oreslang_source_actor_send_capability(heap, 0, payload + 1);
    atomic_fetch_add_explicit(&nested_parent_messages, 1, memory_order_release);
    return next;
}

int main(void) {
    /* Copied peer capability identity survives destruction of caller storage. */
    void* target = oreslang_source_actor_spawn(init_zero, target_handler, 1);
    assert(target != 0);
    void* supplied[1] = {target};
    void* sender = oreslang_source_actor_spawn_with_capabilities(
        init_zero, sender_handler, 1, supplied, 1);
    assert(sender != 0);
    supplied[0] = 0;

    assert(oreslang_source_actor_try_send(sender, 4) == 1);
    wait_for_count(&sender_messages, 1);
    wait_for_count(&target_messages, 1);
    assert(atomic_load_explicit(&target_last, memory_order_relaxed) == 5);
    oreslang_source_actor_stop(sender);
    oreslang_source_actor_stop(target);

    /* Constructor-owned child is private to the parent. Its exact capability
     * can be used by the handler and is physically stopped/joined before the
     * parent allocation is reclaimed. */
    void* parent = oreslang_source_actor_spawn(
        nested_parent_init, nested_parent_handler, 1);
    assert(parent != 0);
    assert(oreslang_source_actor_try_send(parent, 41) == 1);
    wait_for_count(&nested_parent_messages, 1);
    wait_for_count(&nested_child_messages, 1);
    assert(atomic_load_explicit(&nested_child_last, memory_order_relaxed) == 42);
    oreslang_source_actor_stop(parent);

    assert(atomic_load_explicit(
        &nested_parent_messages, memory_order_relaxed) == 1);
    assert(atomic_load_explicit(
        &nested_child_messages, memory_order_relaxed) == 1);
    return 0;
}
