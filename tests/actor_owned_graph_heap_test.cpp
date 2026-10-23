#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"
#include "oreslang/runtime/HungryCarrier.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <limits>
#include <thread>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <optional>

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
    static_assert(Heap::local_link_storage_bytes() == 4,
                  "actor-local graph edges must use compact 32-bit storage");
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
    must_reject([&]{ (void)first.is_linked(alice, reused, old); },
                "query admitted stale target generation");
    must_reject([&]{ (void)first.is_linked(alice, reused,
                   Heap::Handle{old.heap, static_cast<std::uint32_t>(12345), old.generation}); },
                "query admitted invalid target slot");
    must_reject([&]{ (void)first.is_linked(alice, old, reused); },
                "query admitted stale source generation");
    must_reject([&]{ (void)first.copy_out(bob, reused, &data, sizeof(data)); },
                "foreign actor entered heap");
    const auto other = second.allocate(bob, &data, sizeof(data)).value();
    must_reject([&]{ (void)first.is_linked(alice, reused,
                   Heap::Handle{other.heap, other.slot, other.generation}); },
                "query admitted foreign target heap domain");
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


/* Deterministic model-based actor arena stress.  No tracing collector is
 * involved: incoming links must be purged and recycled slots must not revive
 * old non-owning handles.  External ledger is intentionally independent of
 * the implementation's live-node/edge counters. */
static void model_check_graph_reuse_and_stale_handles(std::uint32_t seed) {
    using Heap = ActorOwnedGraphHeap<8, 16, 3>;
    const ActorHeapIdentity owner{71, 19, 5};
    Heap graph(owner);
    std::array<std::optional<Heap::Handle>, 8> live{};
    std::array<std::optional<Heap::Handle>, 8> stale{};
    std::array<std::array<bool, 8>, 8> edges{};
    std::array<std::uint8_t, 8> values{};
    auto random = [&]() noexcept -> std::uint32_t {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    auto choose_live = [&](std::uint32_t hint) -> int {
        for (int j = 0; j < 8; ++j) {
            const int candidate = static_cast<int>((hint + j) % 8);
            if (live[candidate].has_value()) return candidate;
        }
        return -1;
    };
    for (int step = 0; step < 2048; ++step) {
        const auto operation = random() % 8;
        if (operation < 2) {
            const std::uint8_t byte = static_cast<std::uint8_t>(step);
            auto next = graph.allocate(owner, &byte, 1);
            int free_slots = 0;
            for (const auto& handle : live) if (!handle) ++free_slots;
            check(next.has_value() == (free_slots > 0),
                  "bounded graph allocator/model capacity disagreement");
            if (next) {
                const std::size_t slot = next->slot;
                check(slot < live.size() && !live[slot], "allocator reused live node");
                if (stale[slot])
                    check(next->generation > stale[slot]->generation,
                          "graph slot recycled without generation advance");
                live[slot] = *next;
                values[slot] = byte;
            }
        } else if (operation == 2) {
            const int index = choose_live(random());
            if (index >= 0) {
                const auto old = *live[index];
                graph.remove(owner, old);
                stale[index] = old;
                live[index].reset();
                for (int j = 0; j < 8; ++j) {
                    edges[index][j] = false;
                    edges[j][index] = false;
                }
                std::uint8_t probe = 0;
                must_reject([&] { (void)graph.copy_out(owner, old, &probe, 1); },
                            "retired graph handle resurrected after node removal");
            }
        } else if (operation < 6) {
            const int a = choose_live(random());
            const int b = choose_live(random());
            if (a >= 0 && b >= 0) {
                int outgoing = 0;
                for (int j = 0; j < 8; ++j) if (edges[a][j]) ++outgoing;
                const bool expected = !edges[a][b] && outgoing < 3;
                check(graph.link(owner, *live[a], *live[b]) == expected,
                      "graph link/model disagreement");
                if (expected) edges[a][b] = true;
            }
        } else {
            const int index = choose_live(random());
            if (index >= 0) {
                const std::uint8_t changed = static_cast<std::uint8_t>(random());
                graph.write(owner, *live[index], &changed, 1);
                values[index] = changed;
            }
        }
        std::size_t nodes = 0, links = 0;
        for (int a = 0; a < 8; ++a) {
            if (!live[a]) continue;
            ++nodes;
            std::uint8_t copied = 0;
            check(graph.copy_out(owner, *live[a], &copied, 1) == 1 &&
                      copied == values[a], "graph payload changed after reuse");
            for (int b = 0; b < 8; ++b) {
                if (!live[b]) {
                    check(!edges[a][b], "model retained link to dead slot");
                    continue;
                }
                check(graph.is_linked(owner, *live[a], *live[b]) == edges[a][b],
                      "graph adjacency/model disagreement");
                if (edges[a][b]) ++links;
            }
        }
        // A stale handle must remain invalid even after thousands of later
        // allocations and recycling cycles; test across multiple states,
        // not only at the instant of removal.
        if (step % 31 == 0) {
            for (const auto& former : stale) {
                if (former) {
                    std::uint8_t discarded = 0;
                    must_reject([&] {
                        (void)graph.copy_out(owner, *former, &discarded, 1);
                    }, "stale actor handle revived after generation recycling");
                }
            }
        }
        check(graph.live_nodes(owner) == nodes, "graph node accounting drift");
        check(graph.live_edges(owner) == links, "graph edge accounting drift");
    }
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop, bound_options(owner));
    carrier.start();
    carrier.join();
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "model stress lacked trusted retirement proof");
    graph.retire(owner, *proof);
    check(graph.retired(), "model stress failed zero-tracing-GC retirement");
    std::cout << "PASS seeded differential graph ownership and ABA stress\\n";
}


/* Exercise heap-domain exhaustion without creating billions of arenas.
 * The injected atomic is equivalent to the allocator's static counter, so
 * every contending thread must receive either a unique ID or overflow. */
static void test_heap_domains_never_wrap_or_repeat() {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> next{max - 4};
    std::array<std::uint64_t, 12> outcomes{};
    std::array<std::thread, 12> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i]() {
            try {
                outcomes[i] = oreslang::runtime::detail::reserve_actor_graph_heap_id(next);
            } catch (const std::overflow_error&) {
                outcomes[i] = 0;
            }
        });
    }
    for (auto& worker : threads) worker.join();
    std::sort(outcomes.begin(), outcomes.end());
    for (std::size_t i = 0; i < 8; ++i)
        check(outcomes[i] == 0, "exhausted graph domain was incorrectly reissued");
    for (std::size_t i = 0; i < 4; ++i)
        check(outcomes[8 + i] == max - 4 + i, "duplicate or skipped graph heap domain");
    check(next.load() == max, "heap domain counter wrapped after saturation");
    bool saturated = false;
    try {
        (void)oreslang::runtime::detail::reserve_actor_graph_heap_id(next);
    } catch (const std::overflow_error&) {
        saturated = true;
    }
    check(saturated && next.load() == max,
          "exhausted heap domain must permanently reject allocation");
    std::atomic<std::uint64_t> invalid{0};
    saturated = false;
    try {
        (void)oreslang::runtime::detail::reserve_actor_graph_heap_id(invalid);
    } catch (const std::overflow_error&) {
        saturated = true;
    }
    check(saturated && invalid.load() == 0,
          "zero heap domain must never be issued");
}

/* Regression: capacity/template specializations must not each restart their
 * own domain counter. A forged cross-shape handle must remain foreign even
 * when slot and generation happen to be identical. */
static void test_cross_template_graph_heap_domain_isolation() {
    const ActorHeapIdentity owner{91, 27, 3};
    using Left = ActorOwnedGraphHeap<5, 23, 2>;
    using Right = ActorOwnedGraphHeap<6, 29, 2>;
    Left left(owner);
    Right right(owner);
    check(left.heap_id() != right.heap_id(),
          "different graph layout specializations reused a heap domain");
    const std::uint8_t byte = 7;
    const auto lhs = left.allocate(owner, &byte, 1);
    const auto rhs = right.allocate(owner, &byte, 1);
    check(lhs.has_value() && rhs.has_value(), "cross-template setup failed");
    check(lhs->slot == rhs->slot && lhs->generation == rhs->generation,
          "cross-template test did not construct colliding slot/generation");
    const Right::Handle forged{lhs->heap, rhs->slot, rhs->generation};
    std::uint8_t copied = 0;
    must_reject([&] {
        (void)right.copy_out(owner, forged, &copied, 1);
    }, "cross-template foreign graph handle was accepted");
}

// Prevent an actor lease from retaining a dangling pointer to the heap's
// borrow gate. A destructor cannot defer deletion of stack storage, so
// destroying a borrowed heap is a fatal supervisor contract violation.
static void test_destructor_refuses_outstanding_borrows() {
#if defined(__unix__) || defined(__APPLE__)
    pid_t child = fork();
    check(child >= 0, "unable to fork borrowed heap destruction test");
    if (child == 0) {
        std::set_terminate([]() noexcept { std::_Exit(73); });
        const ActorHeapIdentity actor{4001, 3, 9};
        using Heap = ActorOwnedGraphHeap<2, 16, 1>;
        Heap* heap = new Heap(actor);
        auto borrowed = heap->borrow(actor);
        if (!borrowed.has_value()) std::_Exit(74);
        delete heap; // must terminate BEFORE destroying the borrow gate.
        std::_Exit(75);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "borrowed heap waitpid failed");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "destructor silently freed actor heap under a live lease");
#else
    // Non-POSIX targets exercise successful no-borrow destruction below.
#endif

    const ActorHeapIdentity actor{4002, 3, 9};
    using Heap = ActorOwnedGraphHeap<2, 16, 1>;
    {
        Heap heap(actor);
        auto borrowed = heap.borrow(actor);
        check(borrowed.has_value(), "ordinary heap borrow admission");
        borrowed->reset();
    }
    std::cout << "PASS graph actor heap destructor quiescence enforcement\\n";
}

int main() {
    test_destructor_refuses_outstanding_borrows();
    test_cross_template_graph_heap_domain_isolation();
    test_heap_domains_never_wrap_or_repeat();
    for (std::uint32_t seed : {0x51ABCAFEu, 0xA19C7813u,
                               0xC0DEC0DEu, 0xDEADBEEFu}) {
        model_check_graph_reuse_and_stale_handles(seed);
    }
    cycles_reclaim_without_gc();
    stale_generation_and_foreign_heap_rejected();
    bounded_capacity_and_purge();
    test_destructors_are_exactly_once_on_cycles_and_retirement();
}
