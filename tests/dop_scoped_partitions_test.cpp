#include "oreslang/runtime/DopScopedPartitions.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using oreslang::dop::ScopedColumn;
static_assert(!std::is_copy_constructible_v<ScopedColumn<int>>);
static_assert(!std::is_copy_constructible_v<ScopedColumn<int>::WriteLease>);
static_assert(std::is_move_constructible_v<ScopedColumn<int>::WriteLease>);
static_assert(requires (ScopedColumn<int>::ReadLease& loan) {
    loan.for_each([](std::size_t, int) {});
});
template<class L> concept CanTransform = requires (L& loan) { loan.transform([](std::size_t, int x) { return x; }); };
static_assert(!CanTransform<ScopedColumn<int>::ReadLease>);
template<class L> concept HasRawSpan = requires (L& loan) { loan.span(); };
static_assert(!HasRawSpan<ScopedColumn<int>::ReadLease>);
static_assert(!HasRawSpan<ScopedColumn<int>::WriteLease>);
static_assert(requires (ScopedColumn<int>::WriteLease& loan) {
    loan.transform([](std::size_t, int x) { return x; });
});

static void check(bool good) {
    if (!good) throw std::runtime_error("scoped partition assertion failed");
}
template<class E, class F> static void rejected(F&& fn) {
    bool caught = false;
    try { fn(); } catch (const E&) { caught = true; }
    check(caught);
}
int main() {
    ScopedColumn<int> cells(64);
    cells.set(4, 17);
    {
        auto write = cells.write(0, 16);
        auto second = cells.write(16, 16);
        auto reader = cells.read(32, 16);
        int reads = 0;
        reader.for_each([&](std::size_t, int x) { reads += x; });
        check(reads == 0);
        rejected<std::logic_error>([&] { (void)cells.write(15, 2); });
        rejected<std::logic_error>([&] { (void)cells.read(2, 4); });
        rejected<std::logic_error>([&] { (void)cells.write(40, 1); });
        rejected<std::logic_error>([&] { (void)cells.get(4); });
        rejected<std::logic_error>([&] { cells.set(40, 99); });
        rejected<std::logic_error>([&] { cells.resize(65); });
        check(cells.get(55) == 0);
        cells.set(55, 42);
        write.transform([](std::size_t i, int x) { return x + static_cast<int>(i) + 3; });
        second.transform([](std::size_t, int x) { return x + 20; });
        auto same_reader = cells.read(32, 16);
        same_reader.for_each([&](std::size_t, int x) { check(x == 0); });
    }
    check(cells.get(4) == 24); // 17 + 4 + 3
    check(cells.get(16) == 20);
    cells.resize(128);
    rejected<std::out_of_range>([&] { (void)cells.read(127, 2); });
    rejected<std::out_of_range>([&] { (void)cells.write(std::numeric_limits<std::size_t>::max(), 1); });
    { auto blank = cells.read(128, 0); check(blank.size() == 0);
      blank.for_each([](std::size_t, int) { throw std::runtime_error("unexpected"); });
      rejected<std::logic_error>([&] { cells.resize(129); }); }
    cells.resize(64);

    ScopedColumn<std::int64_t> positions(8192);
    std::vector<std::thread> tasks;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int shard = 0; shard < 8; ++shard) {
        auto exclusive = positions.write(static_cast<std::size_t>(shard) * 1024, 1024);
        tasks.emplace_back([loan = std::move(exclusive), &ready, &go, shard]() mutable {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            loan.transform([&](std::size_t, std::int64_t) { return static_cast<std::int64_t>(shard + 1); });
        });
    }
    while (ready.load(std::memory_order_acquire) != 8) std::this_thread::yield();
    rejected<std::logic_error>([&] { (void)positions.read(4096, 1); });
    rejected<std::logic_error>([&] { positions.resize(16384); });
    go.store(true, std::memory_order_release);
    for (auto& thread : tasks) thread.join();
    for (int i = 0; i < 8192; ++i) check(positions.get(i) == i / 1024 + 1);
    positions.resize(16384);
    {
        auto r = positions.read(0, 400);
        auto w = positions.write(400, 100);
        rejected<std::logic_error>([&] { (void)positions.write(399, 2); });
        int total = 0;
        r.for_each([&](std::size_t, std::int64_t x) { total += static_cast<int>(x); });
        check(total == 400);
        w.transform([](std::size_t, std::int64_t x) { return x; });
    }
    {
        auto r1 = positions.read(0, 10);
        auto r2 = positions.read(0, 10);
        r1.release();
        rejected<std::logic_error>([&] { (void)r1.size(); });
        rejected<std::logic_error>([&] { r1.for_each([](std::size_t, std::int64_t) {}); });
        rejected<std::logic_error>([&] { (void)positions.write(2, 1); });
        int checks = 0;
        r2.for_each([&](std::size_t, std::int64_t x) { check(x == 1); ++checks; });
        check(checks == 10);
    }
    auto w = positions.write(2, 1);
    w.transform([](std::size_t, std::int64_t) { return std::int64_t{200}; });
    w.release();
    rejected<std::logic_error>([&] { w.transform([](std::size_t, std::int64_t x) { return x; }); });
    check(positions.get(2) == 200);

    // Callback reentering its own release must not publish an early unlock.
    {
        auto reentrant = positions.write(0, 2);
        reentrant.transform([&](std::size_t i, std::int64_t x) {
            if (i == 0) {
                reentrant.release();
                rejected<std::logic_error>([&] { positions.resize(16385); });
                rejected<std::logic_error>([&] { (void)positions.write(0, 1); });
            }
            return x + 1;
        });
        rejected<std::logic_error>([&] {
            reentrant.transform([](std::size_t, std::int64_t x) { return x; });
        });
        check(positions.get(0) == 2 && positions.get(1) == 2);
    }
    // Release inside a throwing callback must still retire the lease.
    {
        auto throwing = positions.write(20, 2);
        rejected<std::runtime_error>([&] {
            throwing.transform([&](std::size_t, std::int64_t x) -> std::int64_t {
                throwing.release();
                rejected<std::logic_error>([&] { (void)positions.write(20, 1); });
                throw std::runtime_error("failed operation");
                return x;
            });
        });
        auto reuse = positions.write(20, 2);
        reuse.transform([](std::size_t, std::int64_t x) { return x + 1; });
    }
    // Concurrent operations on one shared lease are rejected even though
    // disjoint leases can execute in parallel.
    {
        auto lease = positions.write(100, 4);
        std::atomic<bool> in_callback{false};
        std::atomic<bool> continue_callback{false};
        std::thread worker([&] {
            lease.transform([&](std::size_t, std::int64_t x) {
                in_callback.store(true, std::memory_order_release);
                while (!continue_callback.load(std::memory_order_acquire))
                    std::this_thread::yield();
                return x + 1;
            });
        });
        while (!in_callback.load(std::memory_order_acquire)) std::this_thread::yield();
        rejected<std::logic_error>([&] {
            lease.transform([](std::size_t, std::int64_t x) { return x + 1; });
        });
        rejected<std::logic_error>([&] { positions.resize(16385); });
        continue_callback.store(true, std::memory_order_release);
        worker.join();
        int touched = 0;
        lease.for_each([&](std::size_t, std::int64_t x) { check(x == 2); ++touched; });
        check(touched == 4);
    }
    // Exception in a callback must not leak an in-flight marker.
    {
        auto lease = positions.write(10, 5);
        rejected<std::runtime_error>([&] {
            lease.transform([](std::size_t, std::int64_t x) -> std::int64_t {
                throw std::runtime_error("cancelled");
                return x;
            });
        });
        lease.transform([](std::size_t, std::int64_t x) { return x + 2; });
    }
    // Nested use of same lease must be denied (no overlapping execution).
    {
        auto lease = positions.write(40, 1);
        lease.transform([&](std::size_t, std::int64_t x) {
            rejected<std::logic_error>([&] {
                lease.transform([](std::size_t, std::int64_t y) { return y; });
            });
            return x + 1;
        });
    }
    std::optional<ScopedColumn<int>::ReadLease> pinned;
    {
        auto owner = std::make_unique<ScopedColumn<int>>(4);
        owner->set(0, 99);
        pinned.emplace(owner->read(0, 4));
        owner.reset();
        rejected<std::logic_error>([&] {
            pinned->for_each([](std::size_t, int) {});
        });
    }
    pinned.reset();

    ScopedColumn<int> differential(101);
    struct Live { std::size_t first, end; bool write; };
    std::vector<Live> live;
    std::vector<ScopedColumn<int>::ReadLease> readers;
    std::vector<ScopedColumn<int>::WriteLease> writers;
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
    const auto next = [&]() -> std::uint64_t {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed;
    };
    for (int i = 0; i < 4000; ++i) {
        if (i % 7 == 0) { readers.clear(); writers.clear(); live.clear(); }
        const auto first = static_cast<std::size_t>(next() % 102);
        const auto count = static_cast<std::size_t>(next() % (102 - first));
        const bool write = (next() & 1U) != 0;
        const bool conflict = [&]() {
            for (const auto& r : live)
                if (first < r.end && r.first < first + count && (write || r.write)) return true;
            return false;
        }();
        bool admitted = true;
        try {
            if (write) writers.emplace_back(differential.write(first, count));
            else readers.emplace_back(differential.read(first, count));
        } catch (const std::logic_error&) { admitted = false; }
        check(admitted != conflict);
        if (admitted) live.push_back({first, first + count, write});
    }
}
