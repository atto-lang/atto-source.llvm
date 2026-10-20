#include "oreslang/runtime/ActorOwnedHeap.hpp"
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

using namespace oreslang::runtime;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

using Heap = ActorOwnedHeap<512, 8>;
using Handle = Heap::Handle;
struct Node {
    Handle next{};
    std::int64_t value = 0;
};
static_assert(std::is_trivially_copyable<Node>::value, "node is POD-like");
static_assert(!std::is_copy_constructible<Heap>::value, "heap cannot be cloned");
static_assert(!std::is_move_constructible<Heap>::value, "heap cannot be moved");

static void test_independent_actor_storage_and_cycles() {
    Heap alice(101);
    Heap bob(102);
    auto one = alice.allocate(sizeof(Node), alignof(Node));
    auto two = alice.allocate(sizeof(Node), alignof(Node));
    check(one && two, "two actor-owned nodes must allocate");
    check(alice.write(*one, Node{*two, 11}), "first edge must write");
    check(alice.write(*two, Node{*one, 22}), "reciprocal cycle must write");
    auto first = alice.read<Node>(*one);
    auto second = alice.read<Node>(*two);
    check(first && second, "cyclic arena graph must resolve");
    check(first->next == *two && second->next == *one,
          "cycle IDs must survive roundtrip");
    check(!bob.read<Node>(*one), "cross-actor heap handle must fail closed");
    check(!bob.write(*two, Node{*one, 33}),
          "cross-actor write must not mutate foreign heap");
    check(bob.used() == 0, "cross-actor attack must not allocate");
    check(alice.live_allocations() == 2, "actor allocations tracked separately");
    check(alice.remove(*two), "remove second actor node");
    check(!alice.read<Node>(*two), "removed node handle must stay stale");
    check(!alice.remove(*two), "double-remove must fail");
    auto replacement = alice.allocate(sizeof(Node), alignof(Node));
    check(replacement.has_value(), "reused slot must allocate");
    check(!alice.read<Node>(*two), "reused slot must not resurrect old ID");
    check(alice.write(*replacement, Node{*replacement, 42}),
          "self-cycle must be supported without reference counting");
    check(alice.read<Node>(*replacement)->next == *replacement,
          "self-cycle handle roundtrip");
    check(alice.live_allocations() == 2, "removed slot capacity reclaimed");

    alice.retire_quiesced();
    alice.retire_quiesced();
    check(alice.retired() && alice.live_allocations() == 0,
          "retired heap must be cleared");
    check(!alice.read<Node>(*one) && !alice.read<Node>(*replacement),
          "no prior actor handle may resolve after retirement");
    check(!alice.allocate(8), "retired heap cannot allocate");
    check(bob.allocate(8).has_value(),
          "retiring one actor cannot retire a different actor heap");
}

static void test_bounded_storage_and_invalid_accesses() {
    using Small = ActorOwnedHeap<64, 2>;
    Small heap(7);
    check(!heap.allocate(0), "zero allocation must reject");
    check(!heap.allocate(8, 3), "non-power-of-two alignment must reject");
    check(!heap.allocate(1024), "byte quota must reject");
    check(heap.used() == 0, "failed allocation must leave accounting unchanged");
    auto a = heap.allocate(16, 8);
    auto b = heap.allocate(16, 8);
    check(a && b, "slots must allocate");
    check(!heap.allocate(1), "slot quota must reject");
    check(heap.write(*a, std::uint64_t{123}, 8), "in-bounds write");
    check(!heap.write(*a, std::uint64_t{123}, 10), "out-of-bounds write");
    check(heap.read<std::uint64_t>(*a, 8).value() == 123,
          "in-bounds read");
    check(!heap.read<std::uint64_t>(*a, 10), "out-of-bounds read");
    check(heap.remove(*a), "remove slot");
    auto reused = heap.allocate(8, 8);
    check(reused && !heap.read<std::uint64_t>(*a),
          "slot generation must invalidate stale aliases");
}

// Reconstructing a heap at exactly the same address must not resurrect an
// old actor's handles. This also guards hot reload and actor-ID reuse.
static void test_long_lived_actor_full_drain_recycles_bytes_without_aba() {
    using Tiny = ActorOwnedHeap<64, 2>;
    Tiny heap(9);
    auto old = heap.allocate(16, 8);
    check(old.has_value(), "initial allocation");
    check(heap.remove(*old), "initial deterministic free");
    check(heap.used() == 0, "last free must recycle byte capacity");

    // Without a full-drain reset, a bump allocator runs out after only
    // a handful of request handlers even though it has no live objects.
    for (int n = 0; n < 1000; ++n) {
        auto current = heap.allocate(16, 8);
        check(current.has_value(), "long-lived actor must reuse freed bytes");
        check(!heap.read<std::uint64_t>(*old),
              "stale generation must never resurrect after cursor reset");
        check(heap.read<std::uint64_t>(*current).value() == 0,
              "reused memory must be zeroed");
        check(heap.write(*current, std::uint64_t{123}),
              "recycled allocation remains writable");
        check(heap.remove(*current), "deterministic removal");
        check(heap.used() == 0 && heap.live_allocations() == 0,
              "every completed request releases bump capacity");
    }
}

static void test_heap_address_reuse_aba() {
    alignas(Heap) unsigned char storage[sizeof(Heap)];
    Heap* former = new (storage) Heap(33);
    auto old = former->allocate(8);
    check(old.has_value(), "old heap allocation");
    former->~Heap();
    Heap* fresh = new (storage) Heap(33);
    auto value = fresh->allocate(8);
    check(value.has_value(), "new actor incarnation must allocate");
    check(!fresh->read<std::uint64_t>(*old),
          "new actor heap must reject old-generation handle");
    fresh->~Heap();
}

int main() {
    try {
        test_independent_actor_storage_and_cycles();
        test_bounded_storage_and_invalid_accesses();
        test_heap_address_reuse_aba();
        test_long_lived_actor_full_drain_recycles_bytes_without_aba();
        std::cout << "PASS native actor-owned byte heap contracts\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
