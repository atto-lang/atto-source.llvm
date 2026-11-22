// Independently executable C++17 *host* class identity/receiver contract.
// Does not claim a compiled .atto class or actor/runtime ABI.
#include "InvertedClassHandleContract.hpp"
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using Table = oreslang::dop::contract::InvertedClassHandleContract<8>;
static_assert(!std::is_default_constructible_v<Table::Handle>);
static_assert(!std::is_constructible_v<Table::Handle, std::uint64_t,
                                       std::uint64_t, std::uint32_t, std::uint32_t>);
static_assert(std::is_trivially_copyable_v<Table::Value>);

static void check(bool result, const char* why) {
    if (!result) throw std::runtime_error(why);
}
template<class Error, class F>
static void rejects(F&& f) {
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("expected checked denial");
}
int main() {
    const auto issuer_before =
        oreslang::dop::contract::detail::InvertedClassIssuerSequence::allocate();
    rejects<std::invalid_argument>([] { Table invalid(0); });
    Table table(101);
    oreslang::dop::contract::InvertedClassHandleContract<2> other_capacity(101);
    const auto issuer_after =
        oreslang::dop::contract::detail::InvertedClassIssuerSequence::allocate();
    check(issuer_after == issuer_before + 3,
          "class issuer sequence must span schema capacities without zero-actor leakage");
    (void)other_capacity;
    const auto a = table.push(10, 20);
    const auto b = table.push(42, 100);
    const auto c = table.push(7, 11);
    const auto saved = table.snapshot(b);
    table.move_by(a, 5, 7);
    check(table.snapshot(a).x == 15 && table.snapshot(a).y == 27,
          "receiver update not atomic/value correct");
    table.erase_swap(b);
    check(table.size() == 2, "swap-delete length wrong");
    check(table.snapshot(c).x == 7 && table.snapshot(c).y == 11,
          "survivor reference moved to wrong slot");
    check(saved.x == 42 && saved.y == 100,
          "detached value did not survive erase");
    rejects<std::out_of_range>([&] { (void)table.snapshot(b); });
    auto reused = table.push(99, 77);
    check(table.snapshot(reused).x == 99, "reused row wrong");
    rejects<std::out_of_range>([&] { table.move_by(b, 1, 1); });
    Table foreign_same_actor(101), foreign_actor(202);
    rejects<std::out_of_range>([&] { (void)foreign_same_actor.snapshot(a); });
    rejects<std::out_of_range>([&] { (void)foreign_actor.snapshot(a); });
    rejects<std::out_of_range>([&] { (void)table.snapshot(foreign_actor.push(1,2)); });
    const auto max = std::numeric_limits<std::int64_t>::max();
    auto high = table.push(max, 0);
    rejects<std::overflow_error>([&] { table.move_by(high, 1, 12); });
    check(table.snapshot(high).x == max && table.snapshot(high).y == 0,
          "overflow committed partial receiver mutation");
    const auto min = std::numeric_limits<std::int64_t>::min();
    auto low = table.push(min, 0);
    rejects<std::overflow_error>([&] { table.move_by(low, -1, 12); });
    check(table.snapshot(low).x == min && table.snapshot(low).y == 0,
          "underflow committed partial receiver mutation");
    table.close();
    table.close(); // idempotent
    rejects<std::logic_error>([&] { (void)table.snapshot(a); });
    rejects<std::logic_error>([&] { table.push(1, 2); });
    check(saved.x == 42, "detached value invalidated on close");
    // Even placement-new at an identical address and actor identity must not
    // resurrect a stale handle after the previous owner was destroyed.
    alignas(Table) unsigned char storage[sizeof(Table)];
    auto* previous = new (storage) Table(101);
    const auto obsolete = previous->push(3, 4);
    previous->~Table();
    auto* replacement = new (storage) Table(101);
    replacement->push(8, 9);
    rejects<std::out_of_range>([&] { (void)replacement->snapshot(obsolete); });
    replacement->~Table();

    oreslang::dop::contract::InvertedClassHandleContract<2> full(222);
    auto first = full.push(1, 2);
    full.push(3, 4);
    rejects<std::out_of_range>([&] { full.push(5, 6); });
    full.erase_swap(first);
    full.push(7, 8);
    rejects<std::out_of_range>([&] { (void)full.snapshot(first); });

    // Two receiver lanes are allowed to execute in parallel. All fields in
    // each move are committed under one lock, so no torn snapshots are legal.
    Table concurrent(303);
    auto shared = concurrent.push(0, 0);
    std::atomic<bool> go{false};
    std::atomic<bool> broken{false};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i != 4; ++i)
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned j = 0; j < 1000; ++j)
                concurrent.move_by(shared, 1, 1);
        });
    threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned j = 0; j < 2000; ++j) {
            const auto value = concurrent.snapshot(shared);
            if (value.x != value.y) broken.store(true, std::memory_order_relaxed);
        }
    });
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    check(!broken.load() && concurrent.snapshot(shared).x == 4000 &&
          concurrent.snapshot(shared).y == 4000,
          "receiver threads lost an update or exposed torn state");
    concurrent.erase_swap(shared);
    rejects<std::out_of_range>([&] { (void)concurrent.snapshot(shared); });

    // Owner shutdown racing checked readers is linearized: a read either
    // completes before closure or reports closed owner. It never reads freed
    // storage, and all access after closure must be rejected.
    Table teardown(404);
    auto live = teardown.push(1, 2);
    std::atomic<bool> start_teardown{false};
    std::atomic<unsigned> reads_before_close{0};
    std::atomic<unsigned> denials_after_close{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i) readers.emplace_back([&] {
        while (!start_teardown.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (;;) {
            try {
                const auto value = teardown.snapshot(live);
                if (value.x != 1 || value.y != 2)
                    broken.store(true, std::memory_order_relaxed);
                reads_before_close.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::logic_error&) {
                denials_after_close.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    });
    start_teardown.store(true, std::memory_order_release);
    while (reads_before_close.load(std::memory_order_relaxed) < 50)
        std::this_thread::yield();
    teardown.close();
    for (auto& t : readers) t.join();
    check(!broken.load() && denials_after_close.load() == 2,
          "concurrent owner teardown exposed stale data or failed denial");
    rejects<std::logic_error>([&] { (void)teardown.snapshot(live); });
    return 0;
}
