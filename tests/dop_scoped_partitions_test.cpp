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
static_assert(std::is_same_v<decltype(std::declval<ScopedColumn<int>::ReadLease>().span()),
                             std::span<const int>>);
static_assert(std::is_same_v<decltype(std::declval<ScopedColumn<int>::WriteLease>().span()),
                             std::span<int>>);

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
        auto second = cells.write(16, 16);  // adjacent writers do not overlap
        auto reader = cells.read(32, 16);   // disjoint reader with writers
        check(write.span().data() + 16 == second.span().data());
        check(reader.span().size() == 16);
        rejected<std::logic_error>([&] { (void)cells.write(15, 2); });
        rejected<std::logic_error>([&] { (void)cells.read(2, 4); });
        rejected<std::logic_error>([&] { (void)cells.write(40, 1); });
        rejected<std::logic_error>([&] { (void)cells.get(4); });
        rejected<std::logic_error>([&] { cells.set(40, 99); });
        rejected<std::logic_error>([&] { cells.resize(65); });
        check(cells.get(55) == 0);
        cells.set(55, 42);
        check(cells.get(55) == 42);
        write.span()[4] += 3;
        check(write.span()[4] == 20);
        auto same_reader = cells.read(32, 16);
        check(same_reader.span()[0] == 0);
    }
    check(cells.get(4) == 20);
    cells.resize(128);
    rejected<std::out_of_range>([&] { (void)cells.read(127, 2); });
    rejected<std::out_of_range>([&] { (void)cells.write(std::numeric_limits<std::size_t>::max(), 1); });
    { auto blank = cells.read(128, 0); check(blank.span().empty());
      rejected<std::logic_error>([&] { cells.resize(129); }); }
    cells.resize(64);

    // Actor-independent simultaneous writes to disjoint portions of ONE column.
    ScopedColumn<std::int64_t> positions(8192);
    std::vector<std::thread> tasks;
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    for (int shard = 0; shard < 8; ++shard) {
        auto exclusive = positions.write(static_cast<std::size_t>(shard) * 1024, 1024);
        tasks.emplace_back([lease = std::move(exclusive), &ready, &release, shard]() mutable {
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            for (auto& v : lease.span()) v = shard + 1;
        });
    }
    while (ready.load(std::memory_order_acquire) != 8) std::this_thread::yield();
    rejected<std::logic_error>([&] { (void)positions.read(4096, 1); });
    rejected<std::logic_error>([&] { positions.resize(16384); });
    release.store(true, std::memory_order_release);
    for (auto& thread : tasks) thread.join();  // physical quiescence barrier
    for (int i = 0; i < 8192; ++i) check(positions.get(i) == i / 1024 + 1);
    positions.resize(16384);  // structural mutation after all Tasks joined

    // Writer within range zero may coexist with a nonoverlapping reader.
    { auto r = positions.read(0, 400);
      auto w = positions.write(400, 100);
      rejected<std::logic_error>([&] { (void)positions.write(399, 2); });
      check(r.span()[399] == 1 && w.span()[0] == 1);
    }

    // Releasing a lease must never release another lease.
    {
        auto r1 = positions.read(0, 10);
        auto r2 = positions.read(0, 10);
        r1.release();
        rejected<std::logic_error>([&] { (void)r1.span(); });
        rejected<std::logic_error>([&] { (void)positions.write(2, 1); });
        check(r2.span()[2] == 1);
    }
    auto w = positions.write(2, 1);
    w.span()[0] = 200;
    w.release();
    rejected<std::logic_error>([&] { (void)w.span(); });
    check(positions.get(2) == 200);

    // Owner teardown does not free pinned memory; retirement revokes new spans.
    std::optional<ScopedColumn<int>::ReadLease> pinned;
    {
        auto owner = std::make_unique<ScopedColumn<int>>(4);
        owner->set(0, 99);
        pinned.emplace(owner->read(0, 4));
        check(pinned->span()[0] == 99);
        owner.reset();
        rejected<std::logic_error>([&] { (void)pinned->span(); });
    }
    pinned.reset();

    // Differential interval/admission checker over a deterministic schedule.
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
