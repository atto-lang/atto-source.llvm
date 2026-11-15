#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <limits>
#include <cstdint>
#include <stdexcept>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedGraphHeap;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// A noexcept destructor uses a caller-owned fixed log: no heap allocation
// is needed when the actor runs compiler-provided drop glue.
struct Tracked final {
    int id;
    int* log;
    std::size_t* count;
    Tracked(int value, int* entries, std::size_t* used) noexcept
        : id(value), log(entries), count(used) {}
    ~Tracked() noexcept {
        if (*count >= 8) std::terminate();
        log[(*count)++] = id;
    }
};

static void require_order(const std::array<int, 8>& log,
                          std::size_t count, std::initializer_list<int> expected) {
    check(count == expected.size(), "incorrect number of destructor calls");
    std::size_t i = 0;
    for (int value : expected)
        check(log[i++] == value, "destructor not in reverse construction order");
}

static void reclaimed_slot_drops_in_new_construction_order() {
    using Heap = ActorOwnedGraphHeap<4, 64, 3>;
    const ActorHeapIdentity owner{101, 7, 1};
    std::array<int, 8> log{};
    std::size_t used = 0;
    {
        Heap heap(owner);
        const auto first = heap.emplace_owned<Tracked>(owner, 1, log.data(), &used).value();
        const auto middle = heap.emplace_owned<Tracked>(owner, 2, log.data(), &used).value();
        const auto last = heap.emplace_owned<Tracked>(owner, 3, log.data(), &used).value();
        check(heap.link(owner, first, last) && heap.link(owner, last, first),
              "cyclic graph setup failed");

        // Physical slot 1 is now free, but its historical construction
        // position must NOT be retained when a new object occupies it.
        heap.remove(owner, middle);
        require_order(log, used, {2});
        const auto newest = heap.emplace_owned<Tracked>(owner, 4, log.data(), &used).value();
        check(newest.slot == middle.slot && newest.generation > middle.generation,
              "slot reuse did not increment the generation");
        check(heap.live_nodes(owner) == 3, "live object ledger lost a node");

        // Retirement by owner scope exit is the same erase_all() implementation
        // invoked by a carrier-proven retire() call.
    }
    require_order(log, used, {2, 4, 3, 1});
}

static void exception_scope_runs_exact_once_reverse_teardown() {
    using Heap = ActorOwnedGraphHeap<4, 64, 2>;
    const ActorHeapIdentity owner{102, 7, 1};
    std::array<int, 8> log{};
    std::size_t used = 0;
    bool caught = false;
    try {
        Heap heap(owner);
        check(heap.emplace_owned<Tracked>(owner, 11, log.data(), &used).has_value(),
              "first constructor failed");
        check(heap.emplace_owned<Tracked>(owner, 22, log.data(), &used).has_value(),
              "second constructor failed");
        check(heap.emplace_owned<Tracked>(owner, 33, log.data(), &used).has_value(),
              "third constructor failed");
        throw std::runtime_error("host-only unwinding path");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught, "host scope did not unwind");
    require_order(log, used, {33, 22, 11});
}

static void reused_owner_is_not_dropped_twice() {
    using Heap = ActorOwnedGraphHeap<2, 64, 1>;
    const ActorHeapIdentity owner{103, 7, 1};
    std::array<int, 8> log{};
    std::size_t used = 0;
    {
        Heap heap(owner);
        const auto old = heap.emplace_owned<Tracked>(owner, 5, log.data(), &used).value();
        heap.remove(owner, old);
        check(heap.emplace_owned<Tracked>(owner, 6, log.data(), &used).has_value(),
              "freed slot not reusable");
        check(heap.emplace_owned<Tracked>(owner, 7, log.data(), &used).has_value(),
              "second live owner missing");
    }
    require_order(log, used, {5, 7, 6});
}

#ifndef ORESLANG_GRAPH_DROP_BASELINE_ORACLE
template<class F>
static void require_invalid_host_span(F&& operation, const char* why) {
    bool rejected = false;
    try { operation(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, why);
}

static void private_host_byte_spans_cannot_modify_actor_heap() {
    using Heap = ActorOwnedGraphHeap<2, 64, 1>;
    const ActorHeapIdentity owner{104, 7, 1};
    Heap heap(owner);
    const std::array<unsigned char, 8> source{{1, 2, 3, 4, 5, 6, 7, 8}};
    const auto slot = heap.allocate(owner, source.data(), source.size()).value();
    std::array<unsigned char, 8> copy{};
    auto* control = reinterpret_cast<unsigned char*>(&heap);
    auto* wrapped = reinterpret_cast<unsigned char*>(
        std::numeric_limits<std::uintptr_t>::max() - 3);

    require_invalid_host_span([&] { (void)heap.allocate(owner, control, 8); },
        "allocation read from private actor metadata");
    require_invalid_host_span([&] { heap.write(owner, slot, control, 8); },
        "write read from private actor metadata");
    require_invalid_host_span([&] { (void)heap.copy_out(owner, slot, control, 8); },
        "read overwrote private actor metadata");
    require_invalid_host_span([&] {
        (void)heap.copy_out(owner, slot, control + sizeof(heap) - 1, 8);
    }, "boundary-straddling output overwrote private actor heap");
    require_invalid_host_span([&] { (void)heap.allocate(owner, wrapped, 8); },
        "allocation admitted a wrapping host span");
    require_invalid_host_span([&] { heap.write(owner, slot, wrapped, 8); },
        "write admitted a wrapping host span");
    require_invalid_host_span([&] { (void)heap.copy_out(owner, slot, wrapped, 8); },
        "read admitted a wrapping host span");

    check(heap.live_nodes(owner) == 1, "denied alias corrupted node count");
    check(heap.copy_out(owner, slot, copy.data(), copy.size()) == source.size(),
        "valid copy no longer works");
    check(copy == source, "denied host alias modified live actor payload");
    heap.remove(owner, slot);
    check(heap.live_nodes(owner) == 0, "denied alias corrupted reclamation");
}

#endif // ORESLANG_GRAPH_DROP_BASELINE_ORACLE

int main() {
#ifndef ORESLANG_GRAPH_DROP_BASELINE_ORACLE
    private_host_byte_spans_cannot_modify_actor_heap();
#endif
    reclaimed_slot_drops_in_new_construction_order();
    exception_scope_runs_exact_once_reverse_teardown();
    reused_owner_is_not_dropped_twice();
    std::puts("PASS actor graph private buffer denial, reverse construction order, slot reuse, host unwind, exactly-once drops");
    return 0;
}
