#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"
#include "oreslang/runtime/HungryCarrier.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedGraphHeap;
using oreslang::runtime::HungryCarrier;
using oreslang::runtime::ActorStep;
using oreslang::runtime::ActorMessage;

static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
template <class Fn>
static void must_reject(Fn&& f, const char* why) {
    bool rejected = false;
    try { f(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, why);
}

static ActorStep done_handler(void*, ActorMessage, bool, std::int64_t) noexcept {
    return {};
}
static std::int64_t io_noop(void*, std::int64_t value,
                            const std::atomic<bool>&) noexcept {
    return value;
}
static HungryCarrier::Options bound_options(ActorHeapIdentity owner) {
    HungryCarrier::Options options{};
    options.heap_owner = owner;
    return options;
}

static void cycles_reclaim_without_gc() {
    using Heap = ActorOwnedGraphHeap<16, 32, 4>;
    ActorHeapIdentity owner{9, 7, 1};
    Heap graph(owner);
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop, bound_options(owner));
    carrier.start();
    check(!carrier.retirement_proof().has_value(),
          "heap retirement proof available before carrier join");
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

    auto borrow = graph.borrow(owner);
    check(borrow.has_value(), "failed to obtain a test FFI/continuation lease");
    carrier.join(); // stops carrier, broker, ingress, and drains producer leases
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "no retirement proof after joined carrier");
    must_reject([&]{ graph.retire(owner, *proof); },
                "heap retired with an outstanding FFI/continuation lease");
    check(!graph.retired(), "failed retirement erroneously freed graph storage");
    check(!graph.borrow(owner).has_value(),
          "a racing borrower got in after the retirement gate sealed");
    borrow->reset();
    graph.retire(owner, *proof);
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
                       Heap::Handle{other.heap, other.slot, other.generation}); },
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
    // A proof for a different epoch cannot reclaim this actor heap.
    HungryCarrier foreign(nullptr, done_handler, nullptr, io_noop, bound_options(d));
    foreign.start();
    foreign.join();
    auto foreign_proof = foreign.retirement_proof();
    check(foreign_proof.has_value(), "no joined foreign proof");
    must_reject([&]{ h.retire(actor, *foreign_proof); },
                "foreign/stale actor epoch proof retired live heap");
    check(h.live_nodes(actor) == 2, "stale actor epoch modified heap");
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop, bound_options(actor));
    carrier.start();
    carrier.join();
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "joined native carrier did not mint retirement proof");
    h.retire(actor, *proof);
    std::cout << "PASS fixed-size arena quotas and ABA mitigation\n";
}

struct DroppableActorObject final {
    std::atomic<int>* drops;
    explicit DroppableActorObject(std::atomic<int>* counter) noexcept : drops(counter) {}
    ~DroppableActorObject() noexcept {
        drops->fetch_add(1, std::memory_order_relaxed);
    }
};

static void test_destructors_are_exactly_once_on_cycles_and_retirement() {
    using Heap = ActorOwnedGraphHeap<4, 64, 3>;
    const ActorHeapIdentity owner{14, 21, 2};
    std::atomic<int> drops{0};
    Heap h(owner);

    const auto a = h.emplace_owned<DroppableActorObject>(owner, &drops).value();
    const auto b = h.emplace_owned<DroppableActorObject>(owner, &drops).value();
    check(h.link(owner, a, b) && h.link(owner, b, a),
          "owned object graph mutual cycle not admitted");
    check(h.link(owner, a, a), "owned object self-cycle not admitted");
    std::array<std::byte, 64> invalidCopy{};
    must_reject([&] { (void)h.copy_out(owner, a, invalidCopy.data(),
                                      invalidCopy.size()); },
                "owned C++ object was incorrectly copied out as POD bytes");
    must_reject([&] { h.write(owner, a, invalidCopy.data(), invalidCopy.size()); },
                "owned C++ object was incorrectly overwritten as POD bytes");
    h.remove(owner, a);
    check(drops.load(std::memory_order_relaxed) == 1,
          "explicitly deleted object destructor did not run exactly once");
    const auto c = h.emplace_owned<DroppableActorObject>(owner, &drops).value();
    check(c.slot == a.slot && c.generation > a.generation,
          "droppable actor object slot did not advance generation");
    must_reject([&] { h.remove(owner, a); },
                "stale destructor slot was not rejected");

    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop, bound_options(owner));
    carrier.start();
    carrier.join();
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "missing joined carrier proof for destructor drop");
    h.retire(owner, *proof);
    check(drops.load(std::memory_order_relaxed) == 3,
          "actor retirement did not drop exactly two remaining live objects");
    std::cout << "PASS deterministic native destructor glue and cyclic actor retirement\\n";
}

static void borrowed_nodes_cannot_be_mutated_or_dropped() {
    using Heap = ActorOwnedGraphHeap<4, 64, 3>;
    const ActorHeapIdentity owner{43, 37, 4};
    std::uint32_t data = 73;
    std::atomic<int> drops{0};
    Heap graph(owner);
    const auto a = graph.allocate(owner, &data, sizeof(data)).value();
    const auto b = graph.emplace_owned<DroppableActorObject>(owner, &drops).value();

    auto pinned = graph.borrow(owner);
    check(pinned.has_value(), "failed to acquire borrowed native graph view");
    must_reject([&] { graph.remove(owner, b); },
                "dropped borrowed object while a live FFI lease existed");
    must_reject([&] { graph.write(owner, a, &data, sizeof(data)); },
                "mutated bytes underneath a live FFI lease");
    must_reject([&] { (void)graph.allocate(owner, &data, sizeof(data)); },
                "reused arena allocation while an FFI lease existed");
    must_reject([&] { (void)graph.emplace_owned<DroppableActorObject>(owner, &drops); },
                "constructed object under an outstanding FFI lease");
    must_reject([&] { (void)graph.link(owner, a, b); },
                "mutated graph edges while an FFI lease existed");
    check(graph.live_nodes(owner) == 2 && drops.load() == 0,
          "denied pointer mutation changed object lifetime or drop counters");

    std::uint32_t copied = 0;
    check(graph.copy_out(owner, a, &copied, sizeof(copied)) == sizeof(copied)
                && copied == data,
          "read-only copy_out failed while a shared lease was active");
    pinned->reset();
    check(graph.link(owner, a, b), "graph mutation remained sealed after loan release");
    graph.remove(owner, b);
    check(drops.load() == 1, "owned pointer destructor failed after loan release");
    graph.write(owner, a, &data, sizeof(data));
    graph.remove(owner, a);
    std::cout << "PASS borrowed graph node removal/mutation exclusion\\n";
}

int main() {
    borrowed_nodes_cannot_be_mutated_or_dropped();
    cycles_reclaim_without_gc();
    stale_generation_and_foreign_heap_rejected();
    bounded_capacity_and_purge();
    test_destructors_are_exactly_once_on_cycles_and_retirement();
}
