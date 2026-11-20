#include "oreslang/runtime/OwnedHeap.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using oreslang::runtime::OwnedHeap;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Fn>
static void must_throw(Fn&& fn, const char* message) {
    bool threw = false;
    try { fn(); }
    catch (const std::exception&) { threw = true; }
    check(threw, message);
}

struct Counter {
    std::atomic<int>* destroyed;
    int value;
    Counter(std::atomic<int>& counter, int n) noexcept : destroyed(&counter), value(n) {}
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    ~Counter() noexcept { destroyed->fetch_add(1, std::memory_order_relaxed); }
};
struct Throwing {
    explicit Throwing(bool fail) {
        if (fail) throw std::runtime_error("construction failed");
    }
    ~Throwing() noexcept = default;
};

static_assert(!std::is_copy_constructible<OwnedHeap::Owned<Counter>>::value,
              "native ownership must not be copyable");
static_assert(std::is_move_constructible<OwnedHeap::Owned<Counter>>::value,
              "native ownership must be movable");
static_assert(!std::is_copy_constructible<OwnedHeap::Borrow<Counter, false>>::value,
              "borrow leases must not be copyable");

static void test_deterministic_drops_and_moves() {
    std::atomic<int> destroyed{0};
    OwnedHeap heap(sizeof(Counter) * 2);
    auto first = heap.make<Counter>(destroyed, 41);
    auto second = std::move(first);
    check(!first && second, "move must consume the old owner");
    must_throw([&] { (void)first.borrow(); }, "borrow from moved owner was allowed");
    auto third = heap.make<Counter>(destroyed, 42);
    must_throw([&] { (void)heap.make<Counter>(destroyed, 43); },
               "heap must reject live-byte limit overflow");
    check(heap.stats().live_objects == 2, "wrong live owner count");

    {
        auto shared = second.borrow();
        shared.with([](const Counter& value) { check(value.value == 41, "bad shared read"); });
        auto also_shared = second.borrow();
        must_throw([&] { (void)second.borrow_mut(); }, "exclusive borrow overlaps reads");
        must_throw([&] { second.drop(); }, "explicit drop violated shared borrow");
        also_shared.reset();
        shared.reset();
    }

    {
        auto exclusive = third.borrow_mut();
        exclusive.with([](Counter& value) { value.value = 77; });
        must_throw([&] { (void)third.borrow(); }, "shared borrow overlaps exclusive borrow");
        must_throw([&] { (void)third.borrow_mut(); }, "exclusive overlap allowed");
        exclusive.reset();
        auto read = third.borrow();
        read.with([](const Counter& value) { check(value.value == 77, "mutation was lost"); });
    }

    second.drop();
    second.drop();  // idempotent
    check(destroyed.load() == 1, "unique owner not immediately destroyed");
    check(heap.stats().live_objects == 1, "drop bookkeeping is wrong");
    must_throw([&] { heap.close(); }, "retirement with live allocation succeeded");
    third.drop();
    check(destroyed.load() == 2 && heap.stats().live_bytes == 0,
          "owned values were not deterministically freed");
    heap.close();
    check(heap.stats().closed, "heap was not retired");
    must_throw([&] { (void)heap.make<Counter>(destroyed, 44); },
               "closed heap admitted a new allocation");
}

static void test_constructor_failure_and_quota_rollback() {
    OwnedHeap heap(sizeof(Throwing));
    must_throw([&] { (void)heap.make<Throwing>(true); },
               "throwing constructor unexpectedly succeeded");
    check(heap.stats().live_objects == 0 && heap.stats().live_bytes == 0,
          "failed construction leaked quota");
    auto good = heap.make<Throwing>(false);
    check(heap.stats().live_objects == 1, "successful construction unaccounted");
    good.drop();
    heap.close();
}

static void test_host_borrow_lease_survives_early_owner_destruction() {
    std::atomic<int> destroyed{0};
    OwnedHeap heap(sizeof(Counter));
    auto value = heap.make<Counter>(destroyed, 7);
    auto borrow = value.borrow();
    // Defense-in-depth for a buggy native host: dropping the owner cannot
    // free the object under an outstanding borrow, even if source-level
    // lifetime enforcement has not yet been implemented.
    value = {};
    check(destroyed.load() == 0, "owner destruction caused borrowed UAF");
    must_throw([&] { heap.close(); }, "borrowed allocation lost retirement fence");
    borrow.with([](const Counter& x) { check(x.value == 7, "borrow lost value"); });
    borrow.reset();
    check(destroyed.load() == 1, "last borrow did not free the allocation");
    heap.close();
}

static void test_separate_heap_accounting_and_stress() {
    std::atomic<int> a_drops{0}, b_drops{0};
    OwnedHeap a(sizeof(Counter) * 256), b(sizeof(Counter) * 256);
    std::vector<OwnedHeap::Owned<Counter>> values;
    for (int n = 0; n < 256; n++) {
        values.push_back(a.make<Counter>(a_drops, n));
    }
    auto other = b.make<Counter>(b_drops, 3);
    check(a.stats().live_objects == 256 && b.stats().live_objects == 1,
          "actor heap accounting crossed boundaries");
    for (auto& value : values) value.drop();
    check(a.stats().live_objects == 0 && b.stats().live_objects == 1,
          "local drops changed another heap");
    a.close();
    must_throw([&] { b.close(); }, "second heap closed with live allocation");
    other.drop();
    b.close();
    check(a_drops == 256 && b_drops == 1, "isolation counters wrong");

    OwnedHeap concurrent(sizeof(std::uint64_t) * 8000);
    std::atomic<int> complete{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; i++) {
        workers.emplace_back([&] {
            for (int n = 0; n < 1000; ++n) {
                auto value = concurrent.make<std::uint64_t>(42);
                auto lease = value.borrow();
                lease.with([](const std::uint64_t& x) { check(x == 42, "corrupt owner"); });
                lease.reset();
                value.drop();
            }
            ++complete;
        });
    }
    for (auto& worker : workers) worker.join();
    check(complete == 8 && concurrent.stats().live_objects == 0,
          "concurrent deterministic reclamation failed");
    concurrent.close();
}

static void test_callback_owns_independent_read_lifetime() {
    std::atomic<int> drops{0};
    OwnedHeap heap(sizeof(Counter));
    auto owner = heap.make<Counter>(drops, 23);
    auto lease = owner.borrow();
    lease.with([&](const Counter& value) {
        lease.reset();
        must_throw([&] { owner.drop(); }, "callback reset admitted early drop");
        must_throw([&] { (void)owner.borrow_mut(); }, "active read callback admitted mutable alias");
        owner = {};
        check(drops == 0 && value.value == 23, "callback lost allocation lifetime");
        must_throw([&] { heap.close(); }, "active callback lost heap retirement fence");
    });
    check(drops == 1 && heap.stats().live_objects == 0, "callback pin leaked allocation");
    heap.close();
}

static void test_mutable_callback_reset_and_exception_cleanup() {
    OwnedHeap heap(sizeof(int));
    auto owner = heap.make<int>(7);
    auto lease = owner.borrow_mut();
    must_throw([&] {
        lease.with([&](int& value) {
            lease.reset();
            must_throw([&] { (void)owner.borrow(); }, "mutable callback admitted read alias");
            must_throw([&] { (void)owner.borrow_mut(); }, "mutable callback admitted write alias");
            must_throw([&] { owner.drop(); }, "mutable callback admitted destruction");
            value = 42;
            throw std::runtime_error("callback failure");
        });
    }, "callback exception swallowed");
    auto next = owner.borrow_mut();
    next.with([](int& value) { check(value == 42, "callback exception corrupted value"); });
    next.reset();
    owner.drop();
    heap.close();
}

static void test_same_mutable_lease_cannot_run_callbacks_concurrently() {
    OwnedHeap heap(sizeof(int));
    auto owner = heap.make<int>(7);
    auto lease = owner.borrow_mut();
    std::promise<void> entered, release;
    auto ready = release.get_future().share();
    std::thread first([&] { lease.with([&](int&) { entered.set_value(); ready.wait(); }); });
    entered.get_future().wait();
    bool denied = false;
    try { lease.with([](int&) {}); } catch (const std::logic_error&) { denied = true; }
    release.set_value();
    first.join();
    check(denied, "same mutable lease admitted concurrent callbacks");
    lease.reset();
    owner.drop();
    heap.close();
}

int main() {
    test_callback_owns_independent_read_lifetime();
    test_mutable_callback_reset_and_exception_cleanup();
    test_same_mutable_lease_cannot_run_callbacks_concurrently();
    test_deterministic_drops_and_moves();
    test_constructor_failure_and_quota_rollback();
    test_host_borrow_lease_survives_early_owner_destruction();
    test_separate_heap_accounting_and_stress();
    std::cout << "PASS: native unique ownership, moves, borrow exclusivity, "
                 "exception rollback, delayed lease cleanup, actor-local accounting, "
                 "close fencing, and concurrent deterministic drops\n";
}
