#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedGraphHeap;

static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
template <class Fn>
static void must_reject(Fn&& f, const char* why) {
    bool rejected = false;
    try { f(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, why);
}

static void cycles_reclaim_without_gc() {
    using Heap = ActorOwnedGraphHeap<16, 32, 4>;
    ActorHeapIdentity owner{9, 7, 1};
    Heap graph(owner);
    std::array<std::uint8_t, 3> a_data{1, 2, 3}, b_data{4, 5, 6};
    const auto a = graph.allocate(owner, a_data.data(), a_data.size()).value();
    const auto b = graph.allocate(owner, b_data.data(), b_data.size()).value();
    check(graph.link(owner, a, b), "a->b failed");
    check(graph.link(owner, b, a), "b->a failed");
    check(graph.link(owner, a, a), "self cycle failed");
    check(!graph.link(owner, a, a), "duplicate edge admitted");
    check(graph.live_edges(owner) == 3, "cycle edges not tracked");
    check(graph.live_nodes(owner) == 2, "cycle nodes not tracked");

    std::array<std::uint8_t, 32> output{};
    check(graph.copy_out(owner, a, output.data(), output.size()) == 3,
          "native payload length mismatch");
    check(output[0] == 1 && output[2] == 3, "native payload altered");

    must_reject([&]{ graph.retire(owner, false); },
                "heap retired with outstanding continuation/borrow precondition");
    check(graph.live_nodes(owner) == 2, "pre-quiescence failure altered heap");
    graph.retire(owner, true);
    check(graph.retired(), "cyclic heap not retired deterministically");
    must_reject([&]{ (void)graph.copy_out(owner, a, output.data(), output.size()); },
                "stale handle succeeded after heap retirement");
    std::cout << "PASS deterministic actor-owned cycle retirement\n";
}

static void stale_generation_and_foreign_heap_rejected() {
    using Heap = ActorOwnedGraphHeap<4, 16, 3>;
    ActorHeapIdentity alice{1, 11, 3};
    ActorHeapIdentity bob{1, 12, 3};
    Heap first(alice), second(bob);
    std::uint64_t data = 77;
    const auto old = first.allocate(alice, &data, sizeof(data)).value();
    check(first.link(alice, old, old), "cannot add self-reference");
    first.remove(alice, old);
    check(first.live_edges(alice) == 0, "self edge leaked when freed");
    must_reject([&]{ (void)first.copy_out(alice, old, &data, sizeof(data)); },
                "stale reference resolved");
    const auto reused = first.allocate(alice, &data, sizeof(data)).value();
    check(reused.slot == old.slot && reused.generation > old.generation,
          "reused slot did not advance generation");
    must_reject([&]{ (void)first.copy_out(bob, reused, &data, sizeof(data)); },
                "foreign actor entered heap");
    const auto other = second.allocate(bob, &data, sizeof(data)).value();
    must_reject([&]{ (void)first.link(alice, reused,
                       typename Heap::Handle{other.heap, other.slot, other.generation}); },
                "cross-actor graph edge accepted");
    check(first.live_nodes(alice) == 1, "failed authority check changed state");
    std::cout << "PASS stale generation and cross-actor handles\n";
}

static void bounded_capacity_and_purge() {
    using Heap = ActorOwnedGraphHeap<2, 4, 1>;
    ActorHeapIdentity actor{1, 3, 9};
    Heap h(actor);
    const std::uint32_t data = 1234;
    const auto a = h.allocate(actor, &data, sizeof(data)).value();
    const auto b = h.allocate(actor, &data, sizeof(data)).value();
    check(!h.allocate(actor, &data, sizeof(data)).has_value(),
          "node capacity overflow accepted");
    check(h.link(actor, a, b), "a->b failed");
    check(!h.link(actor, a, a), "edge capacity overflow accepted");
    h.remove(actor, b);
    check(h.live_edges(actor) == 0, "incoming stale graph link not purged");
    const auto c = h.allocate(actor, &data, sizeof(data)).value();
    check(c.slot == b.slot && c.generation > b.generation, "recycled slot ABA");
    check(h.link(actor, a, c), "bounded edge not reusable");
    const auto d = ActorHeapIdentity{1, 3, 10};
    must_reject([&]{ h.retire(d, true); }, "stale actor epoch retired live heap");
    check(h.live_nodes(actor) == 2, "stale actor epoch modified heap");
    h.retire(actor, true);
    std::cout << "PASS fixed-size arena quotas and ABA mitigation\n";
}

int main() {
    cycles_reclaim_without_gc();
    stale_generation_and_foreign_heap_rejected();
    bounded_capacity_and_purge();
}
