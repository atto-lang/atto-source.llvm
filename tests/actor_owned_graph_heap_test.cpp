#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"
#include "oreslang/runtime/HungryCarrier.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

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
static HungryCarrier::Options bound_options(
        ActorHeapIdentity owner,
        std::shared_ptr<oreslang::runtime::ActorHeapRetirementCohort> cohort = {}) {
    HungryCarrier::Options options{};
    options.heap_owner = owner;
    options.retirement_cohort = std::move(cohort);
    return options;
}

static void retirement_cohort_registry_does_not_grow_with_actor_churn() {
    using Cohort = oreslang::runtime::ActorHeapRetirementCohort;
    const auto baseline = Cohort::registry_entries_for_diagnostics();
    for (std::uint64_t generation = 1; generation <= 4096; ++generation) {
        {
            auto cohort = Cohort::create(
                ActorHeapIdentity{501, 41, generation}, 1);
            check(cohort->cohort_id() != 0,
                  "actor churn cohort received zero identity");
        }
        // Every new create sweeps the prior dead weak entry, so the registry
        // remains bounded by pre-existing live cohorts plus this iteration.
        check(Cohort::registry_entries_for_diagnostics() <= baseline + 1,
              "retirement cohort registry grew with dead actor generations");
    }
    auto survivor = Cohort::create(ActorHeapIdentity{501, 42, 1}, 1);
    check(Cohort::registry_entries_for_diagnostics() <= baseline + 1,
          "final cohort sweep retained dead actor-generation keys");
    (void)survivor;
}

static void multi_lane_cohort_requires_every_carrier_to_join() {
    using Heap = ActorOwnedGraphHeap<4, 32, 2>;
    const ActorHeapIdentity owner{109, 71, 5};
    auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(owner, 3);
    Heap graph(cohort);
    std::uint64_t payload = 17;
    check(graph.allocate(owner, &payload, sizeof(payload)).has_value(),
          "multi-lane cohort graph allocation failed");

    HungryCarrier first(nullptr, done_handler, nullptr, io_noop,
                        bound_options(owner, cohort));
    HungryCarrier second(nullptr, done_handler, nullptr, io_noop,
                         bound_options(owner, cohort));
    HungryCarrier third(nullptr, done_handler, nullptr, io_noop,
                        bound_options(owner, cohort));
    check(cohort->registered_lanes() == 3,
          "retirement cohort did not register all expected lanes");

    bool fourthRejected = false;
    try {
        HungryCarrier extra(nullptr, done_handler, nullptr, io_noop,
                            bound_options(owner, cohort));
    } catch (const std::invalid_argument&) {
        fourthRejected = true;
    }
    check(fourthRejected, "retirement cohort admitted an unexpected fourth lane");

    bool duplicateCohortRejected = false;
    try {
        (void)oreslang::runtime::ActorHeapRetirementCohort::create(owner, 1);
    } catch (const std::logic_error&) {
        duplicateCohortRejected = true;
    }
    check(duplicateCohortRejected,
          "same actor generation admitted two live retirement cohorts");

    first.start();
    second.start();
    third.start();
    first.join();
    check(!first.retirement_proof().has_value() &&
          cohort->joined_lanes() == 1,
          "one of three joined lanes fabricated actor-wide retirement proof");
    second.join();
    check(!second.retirement_proof().has_value() &&
          cohort->joined_lanes() == 2,
          "two of three joined lanes fabricated actor-wide retirement proof");
    third.join();
    const auto proof = third.retirement_proof();
    check(proof.has_value() && proof->lanes() == 3 &&
          proof->cohort_id() == graph.retirement_cohort_id(),
          "all three lanes did not mint the bound cohort retirement proof");
    check(first.retirement_proof().has_value() &&
          second.retirement_proof().has_value(),
          "fully quiesced cohort proof not visible from already-joined lanes");
    graph.retire(owner, *proof);
    check(graph.retired(), "three-lane actor graph did not retire after quorum");
}

static void cycles_reclaim_without_gc() {
    using Heap = ActorOwnedGraphHeap<16, 32, 4>;
    ActorHeapIdentity owner{9, 7, 1};
    auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(owner, 1);
    Heap graph(cohort);
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop,
                          bound_options(owner, cohort));
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

// Every distinct C++ heap template specialization must share a single
// numeric identity namespace. Re-encoding a handle into a different layout
// cannot trick its recipient into treating a foreign slot as its own.
static void heap_identity_never_wraps_and_remains_unique_under_races() {
    using oreslang::runtime::detail::ActorGraphHeapIdSource;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> near_exhausted{max - 1};
    check(ActorGraphHeapIdSource::mint_from(near_exhausted) == max - 1,
          "penultimate heap ID unavailable");
    check(ActorGraphHeapIdSource::mint_from(near_exhausted) == max,
          "last valid heap ID unavailable");
    bool denied = false;
    try { (void)ActorGraphHeapIdSource::mint_from(near_exhausted); }
    catch (const std::overflow_error&) { denied = true; }
    check(denied && near_exhausted.load() == 0,
          "exhausted ID source wrapped around and could resurrect old handles");

    using Small = ActorOwnedGraphHeap<2, 8, 1>;
    using Large = ActorOwnedGraphHeap<8, 64, 4>;
    const ActorHeapIdentity owner{90, 29, 4};
    std::array<std::uint64_t, 4096> ids{};
    std::array<std::thread, 8> workers;
    std::atomic<bool> start{false};
    for (std::size_t thread = 0; thread < workers.size(); ++thread) {
        workers[thread] = std::thread([&, thread] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::size_t i = thread * 512; i < (thread + 1) * 512; ++i) {
                if (i % 2 == 0) {
                    Small heap(owner);
                    ids[i] = heap.heap_id();
                } else {
                    Large heap(owner);
                    ids[i] = heap.heap_id();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) thread.join();
    std::sort(ids.begin(), ids.end());
    check(ids.front() != 0 &&
          std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "cross-layout concurrent graph construction reused a heap identity");
}

static void heap_identity_unique_across_graph_specializations() {
    using Small = ActorOwnedGraphHeap<2, 8, 1>;
    using Large = ActorOwnedGraphHeap<8, 64, 4>;
    const ActorHeapIdentity owner{90, 29, 4};
    std::uint64_t last = 0;
    for (int i = 0; i < 128; ++i) {
        Small small(owner);
        Large large(owner);
        check(small.heap_id() > last && large.heap_id() > small.heap_id(),
              "graph heap IDs not globally monotonic across layouts");
        last = large.heap_id();
        const std::uint64_t bytes = static_cast<std::uint64_t>(i);
        const auto smallNode = small.allocate(owner, &bytes, sizeof(bytes)).value();
        const auto largeNode = large.allocate(owner, &bytes, sizeof(bytes)).value();
        check(smallNode.heap != largeNode.heap,
              "two graph layouts share the same heap identity");
        Large::Handle forged{smallNode.heap, smallNode.slot, smallNode.generation};
        std::uint64_t copied = 0;
        must_reject([&] { (void)large.copy_out(owner, forged, &copied, sizeof(copied)); },
                    "cross-layout forged graph handle accepted");
    }
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
                "stale generation target accepted by graph edge query");
    must_reject([&]{ (void)first.is_linked(alice, old, reused); },
                "stale generation source accepted by graph edge query");
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
    auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(actor, 1);
    Heap h(cohort);
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
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop,
                          bound_options(actor, cohort));
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

// A three-carrier actor generation may reclaim a graph only after ALL
// independent physical joins publish the cohort quorum. Concurrent acks
// must be monotonic and the graph destructor must never double-drop storage.
static void concurrent_three_lane_join_reclaims_once() {
    using Heap = ActorOwnedGraphHeap<4, 64, 3>;
    for (std::uint64_t iteration = 1; iteration <= 24; ++iteration) {
        const ActorHeapIdentity owner{291, 51, iteration};
        auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(owner, 3);
        Heap graph(cohort);
        std::atomic<int> drops{0};
        check(graph.emplace_owned<DroppableActorObject>(owner, &drops).has_value(),
              "three-lane graph object allocation failed");
        HungryCarrier a(nullptr, done_handler, nullptr, io_noop,
                        bound_options(owner, cohort));
        HungryCarrier b(nullptr, done_handler, nullptr, io_noop,
                        bound_options(owner, cohort));
        HungryCarrier c(nullptr, done_handler, nullptr, io_noop,
                        bound_options(owner, cohort));
        a.start(); b.start(); c.start();
        check(!a.retirement_proof().has_value(),
              "live three-lane cohort issued a pre-join retirement proof");
        std::atomic<bool> go{false};
        auto join_lane = [&](HungryCarrier& lane) {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            lane.join();
        };
        std::thread first(join_lane, std::ref(a));
        std::thread second(join_lane, std::ref(b));
        std::thread third(join_lane, std::ref(c));
        go.store(true, std::memory_order_release);
        first.join(); second.join(); third.join();
        check(cohort->registered_lanes() == 3 && cohort->joined_lanes() == 3,
              "concurrent three-lane join missed an expected physical lane");
        check(cohort->ready_for_retirement(), "joined cohort did not become quiescent");
        const auto proof = a.retirement_proof();
        check(proof.has_value() && proof->cohort_id() == graph.retirement_cohort_id(),
              "joined lanes minted an incorrect cohort retirement proof");
        graph.retire(owner, *proof);
        check(graph.retired() && drops.load(std::memory_order_relaxed) == 1,
              "three-lane reclamation did not execute owned destructor exactly once");
        must_reject([&] { graph.retire(owner, *proof); },
                    "duplicate three-lane retirement reentered destructor glue");
    }
}

static void test_destructors_are_exactly_once_on_cycles_and_retirement() {
    using Heap = ActorOwnedGraphHeap<4, 64, 3>;
    const ActorHeapIdentity owner{14, 21, 2};
    auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(owner, 1);
    std::atomic<int> drops{0};
    Heap h(cohort);

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

    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop,
                          bound_options(owner, cohort));
    carrier.start();
    carrier.join();
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "missing joined carrier proof for destructor drop");
    h.retire(owner, *proof);
    check(drops.load(std::memory_order_relaxed) == 3,
          "actor retirement did not drop exactly two remaining live objects");
    std::cout << "PASS deterministic native destructor glue and cyclic actor retirement\\n";
}

// A borrowed graph remains readable, but no writer may change a node or
// retire it until all external FFI/continuation leases have been released.
static void live_loans_fence_mutations_and_destructor() {
    using Heap = ActorOwnedGraphHeap<4, 64, 2>;
    const ActorHeapIdentity owner{18, 19, 1};
    std::atomic<int> drops{0};
    Heap graph(owner);
    std::uint64_t payload = 42;
    const auto a = graph.allocate(owner, &payload, sizeof(payload)).value();
    const auto b = graph.emplace_owned<DroppableActorObject>(owner, &drops).value();
    auto loan = graph.borrow(owner);
    check(loan.has_value(), "external graph loan not acquired");
    must_reject([&] { (void)graph.allocate(owner, &payload, sizeof(payload)); },
                "allocation raced a live external graph loan");
    must_reject([&] { (void)graph.emplace_owned<DroppableActorObject>(owner, &drops); },
                "owned construction raced a live graph loan");
    must_reject([&] { graph.write(owner, a, &payload, sizeof(payload)); },
                "graph byte write raced a live graph loan");
    must_reject([&] { (void)graph.link(owner, a, b); },
                "graph link mutation raced a live loan");
    must_reject([&] { graph.remove(owner, b); },
                "destructor ran while a graph lease remained live");
    check(drops.load(std::memory_order_relaxed) == 0,
          "failed mutation unexpectedly invoked a destructor");
    loan->reset();
    graph.write(owner, a, &payload, sizeof(payload));
    check(graph.link(owner, a, b), "mutation did not resume after lease end");
    graph.remove(owner, b);
    check(drops.load(std::memory_order_relaxed) == 1,
          "delayed unique-owner destructor did not execute exactly once");

    // Destruction of an unretired heap with active leases must fail closed.
    // A lease contains a pointer to the gate and would otherwise become a
    // dangling native pointer when heap storage is destroyed. Use a child
    // to test the fatal invariant without killing the full native CTest.
    const pid_t child = fork();
    check(child >= 0, "fork failed for destructor lifetime gate");
    if (child == 0) {
        std::set_terminate([] { std::_Exit(86); });
        auto* doomed = new Heap(owner);
        auto still_live = doomed->borrow(owner);
        if (!still_live) std::_Exit(87);
        delete doomed; // expected: terminate before reclaim/free
        std::_Exit(88); // forbidden success
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "child wait failed");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 86,
          "live external graph loan did not prevent heap destruction");
}

static void standalone_borrow_gate_destruction_fail_closed() {
    const ActorHeapIdentity owner{18, 22, 4};
    for (bool hold_mutation : {false, true}) {
        const pid_t child = fork();
        check(child >= 0, "fork failed for standalone gate lifetime test");
        if (child == 0) {
            std::set_terminate([] { std::_Exit(86); });
            auto* gate = new oreslang::runtime::ActorHeapBorrowGate(owner);
            auto lease = hold_mutation ?
                std::optional<oreslang::runtime::ActorHeapBorrowGate::Lease>{} :
                gate->acquire(owner);
            auto mutation = hold_mutation ? gate->begin_mutation(owner) :
                              std::optional<oreslang::runtime::ActorHeapBorrowGate::Mutation>{};
            if (hold_mutation ? !mutation : !lease) std::_Exit(87);
            delete gate; // MUST terminate while lease/writer still references it
            std::_Exit(88);
        }
        int status = 0;
        check(waitpid(child, &status, 0) == child, "child wait failed");
        check(WIFEXITED(status) && WEXITSTATUS(status) == 86,
              "standalone gate was freed with outstanding lease or writer");
    }
}

static void atomic_mutation_vs_loan_races() {
    const ActorHeapIdentity owner{18, 21, 4};
    {
        oreslang::runtime::ActorHeapBorrowGate gate(owner);
        auto writing = gate.begin_mutation(owner);
        check(writing.has_value(), "failed to begin writer before seal");
        check(!gate.seal_and_quiesced(owner), "retirement admitted active graph writer");
        check(!gate.acquire(owner).has_value(),
              "racing graph reader entered a sealed gate");
        writing->reset();
        check(gate.quiesced(), "ended writer did not permit quiescent retirement");
    }
    for (int n = 0; n < 128; ++n) {
        oreslang::runtime::ActorHeapBorrowGate gate(owner);
        std::atomic<bool> go{false};
        std::atomic<int> examined{0};
        std::atomic<int> winners{0};
        std::thread reader([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto lease = gate.acquire(owner);
            if (lease) winners.fetch_add(1, std::memory_order_relaxed);
            examined.fetch_add(1, std::memory_order_release);
            while (examined.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
        });
        std::thread writer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto mutation = gate.begin_mutation(owner);
            if (mutation) winners.fetch_add(1, std::memory_order_relaxed);
            examined.fetch_add(1, std::memory_order_release);
            while (examined.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
        });
        go.store(true, std::memory_order_release);
        reader.join();
        writer.join();
        check(winners.load(std::memory_order_relaxed) == 1,
              "a graph writer and external borrower both entered concurrently");
        check(gate.seal_and_quiesced(owner), "borrow gate failed to drain");
        check(!gate.acquire(owner).has_value(), "borrow after retirement accepted");
        check(!gate.begin_mutation(owner).has_value(), "mutation after retirement accepted");
    }
}

// Model-test edge accounting under slot reuse, self-cycles and partial
// deletion. The model has independent edge and liveness storage.
static void graph_accounting_model_stress() {
    using Heap = ActorOwnedGraphHeap<6, 8, 3>;
    using Handle = Heap::Handle;
    const ActorHeapIdentity owner{45, 71, 9};
    Heap graph(owner);
    std::array<std::optional<Handle>, 6> live{};
    std::array<std::array<bool, 6>, 6> edges{};
    std::uint32_t seed = 0x6f726573;
    auto random = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (int iteration = 0; iteration < 6000; ++iteration) {
        const auto action = random() % 4;
        const auto first = static_cast<std::size_t>(random() % 6);
        const auto second = static_cast<std::size_t>(random() % 6);
        if (action == 0) {
            std::uint64_t bytes = static_cast<std::uint64_t>(iteration);
            const auto result = graph.allocate(owner, &bytes, sizeof(bytes));
            bool has_free = false;
            for (const auto& cell : live) if (!cell) has_free = true;
            check(result.has_value() == has_free, "model allocation capacity disagrees");
            if (result) {
                check(!live[result->slot].has_value(), "reused occupied graph slot");
                live[result->slot] = *result;
                for (auto& row : edges) row[result->slot] = false;
                edges[result->slot].fill(false);
            }
        } else if (action == 1 && live[first] && live[second]) {
            std::size_t outgoing = 0;
            for (bool linked : edges[first]) if (linked) ++outgoing;
            const bool expected = !edges[first][second] && outgoing < 3;
            const bool actual = graph.link(owner, *live[first], *live[second]);
            check(expected == actual, "bounded graph model link disagrees");
            if (actual) edges[first][second] = true;
        } else if (action == 2 && live[first]) {
            const auto old = *live[first];
            graph.remove(owner, old);
            live[first].reset();
            for (auto& row : edges) row[first] = false;
            edges[first].fill(false);
            must_reject([&] { graph.remove(owner, old); },
                        "stale recycled actor graph slot accepted");
        } else if (live[first] && live[second]) {
            check(graph.is_linked(owner, *live[first], *live[second]) ==
                    edges[first][second], "model edge query disagrees");
        }
        std::size_t model_nodes = 0, model_edges = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            if (live[i]) ++model_nodes;
            for (bool linked : edges[i]) if (linked) ++model_edges;
        }
        check(graph.live_nodes(owner) == model_nodes, "graph active node accounting diverged");
        check(graph.live_edges(owner) == model_edges, "graph edge accounting diverged");
    }
}

static void concurrent_retirement_drops_once() {
    using Heap = ActorOwnedGraphHeap<4, 64, 2>;
    const ActorHeapIdentity owner{89, 29, 3};
    auto cohort = oreslang::runtime::ActorHeapRetirementCohort::create(owner, 1);
    Heap graph(cohort);
    std::atomic<int> drops{0};
    const auto object = graph.emplace_owned<DroppableActorObject>(owner, &drops);
    check(object.has_value(), "no tracked native object for retirement race");
    HungryCarrier carrier(nullptr, done_handler, nullptr, io_noop,
                          bound_options(owner, cohort));
    carrier.start();
    carrier.join();
    const auto proof = carrier.retirement_proof();
    check(proof.has_value(), "joined actor failed to issue retirement authority");
    std::atomic<bool> go{false};
    std::atomic<int> successes{0};
    std::atomic<int> denials{0};
    auto retire_one = [&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            graph.retire(owner, *proof);
            successes.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::logic_error&) {
            denials.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread a(retire_one), b(retire_one);
    go.store(true, std::memory_order_release);
    a.join();
    b.join();
    check(successes.load(std::memory_order_relaxed) == 1 &&
          denials.load(std::memory_order_relaxed) == 1,
          "two supervisors both reclaimed or both denied actor retirement");
    check(drops.load(std::memory_order_relaxed) == 1,
          "native object destructor must run exactly once across double retirement");
    check(graph.retired(), "successful actor reclamation did not publish retirement");
}

// Concurrency-configured actors may admit several carrier threads.
// A read must never race the byte write or graph-edge mutation even when
// all callers present the correct actor incarnation authority. Conflicts
// fail closed instead of unsafely peeking into mutable graph slots.
static void concurrent_graph_read_write_links_are_exclusive() {
    using Heap = ActorOwnedGraphHeap<4, 16, 2>;
    const ActorHeapIdentity owner{22, 31, 1};
    Heap graph(owner);
    std::uint64_t value = 0;
    const auto first = graph.allocate(owner, &value, sizeof(value)).value();
    const auto second = graph.allocate(owner, &value, sizeof(value)).value();
    std::atomic<bool> start{false};
    std::atomic<int> successfulWrites{0}, successfulReads{0},
                     edgesAdded{0}, conflictDenials{0};
    auto await_start = [&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    std::thread writer([&] {
        await_start();
        for (std::uint64_t i = 1; i <= 3000; ++i) {
            try {
                graph.write(owner, first, &i, sizeof(i));
                successfulWrites.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::logic_error&) {
                conflictDenials.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread reader([&] {
        await_start();
        for (int i = 0; i < 3000; ++i) {
            std::uint64_t observed = 0;
            try {
                graph.copy_out(owner, first, &observed, sizeof(observed));
                check(observed <= 3000, "concurrent graph reader observed torn payload");
                successfulReads.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::logic_error&) {
                conflictDenials.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread linker([&] {
        await_start();
        for (int i = 0; i < 3000; ++i) {
            try {
                if (i % 2 == 0) {
                    if (graph.link(owner, first, second))
                        edgesAdded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    (void)graph.is_linked(owner, first, second);
                }
            } catch (const std::logic_error&) {
                conflictDenials.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();
    linker.join();
    check(successfulReads.load() + successfulWrites.load() +
              conflictDenials.load() > 0, "concurrent graph stress ran no operations");
    check(edgesAdded.load() <= 1, "concurrent duplicate graph link admitted");
    check(graph.live_nodes(owner) == 2, "concurrent graph lost owned nodes");
    check(graph.live_edges(owner) ==
          static_cast<std::size_t>(edgesAdded.load()), "concurrent edge ledger corrupted");
}

int main() {
    retirement_cohort_registry_does_not_grow_with_actor_churn();
    multi_lane_cohort_requires_every_carrier_to_join();
    concurrent_three_lane_join_reclaims_once();
    heap_identity_never_wraps_and_remains_unique_under_races();
    heap_identity_unique_across_graph_specializations();
    concurrent_graph_read_write_links_are_exclusive();
    concurrent_retirement_drops_once();
    graph_accounting_model_stress();
    standalone_borrow_gate_destruction_fail_closed();
    atomic_mutation_vs_loan_races();
    live_loans_fence_mutations_and_destructor();
    cycles_reclaim_without_gc();
    stale_generation_and_foreign_heap_rejected();
    bounded_capacity_and_purge();
    test_destructors_are_exactly_once_on_cycles_and_retirement();
}
