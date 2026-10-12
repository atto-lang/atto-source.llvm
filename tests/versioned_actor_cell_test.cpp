#include "oreslang/runtime/VersionedActorCell.hpp"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) std::abort(); } while (false)

int main() {
    using Cell = oreslang::VersionedActorCell;
    Cell cell(5);
    auto old = cell.snapshot();
    CHECK(old.value() == 5);
    CHECK(cell.try_commit(old, 6) == Cell::Commit::Committed);
    CHECK(cell.try_commit(old, 7) == Cell::Commit::Conflict);
    auto next = cell.snapshot();
    CHECK(next.value() == 6);
    CHECK(cell.try_commit(next, 5) == Cell::Commit::Committed); // value ABA
    CHECK(cell.try_commit(old, 8) == Cell::Commit::Conflict);
    Cell foreign(5);
    CHECK(foreign.try_commit(cell.snapshot(), 7) == Cell::Commit::Foreign);

    // Two racing logical continuations on one version: exactly one succeeds.
    Cell parallel(0);
    auto baseline = parallel.snapshot();
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&, i] {
            if (parallel.try_commit(baseline, static_cast<std::int64_t>(i + 1))
                    == Cell::Commit::Committed) ++winners;
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(winners == 1);

    Cell exhausted(0, static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()));
    CHECK(exhausted.try_commit(exhausted.snapshot(), 4) == Cell::Commit::Exhausted);
    // A snapshot pins the old identity, even after cell destruction and
    // potential allocation-address reuse by a replacement actor instance.
    std::optional<Cell::Snapshot> old_incarnation;
    {
        auto prior = std::make_unique<Cell>(10);
        old_incarnation = prior->snapshot();
    }
    Cell replacement(10);
    CHECK(replacement.try_commit(*old_incarnation, 20) == Cell::Commit::Foreign);

    // Closing races with an outstanding continuation; either the last
    // commit or retire may linearize first, but no commit succeeds afterward.
    Cell closing(0);
    auto ready = closing.snapshot();
    std::thread canceller([&] { closing.retire(); });
    auto race = closing.try_commit(ready, 99);
    canceller.join();
    CHECK(race == Cell::Commit::Committed || race == Cell::Commit::Retired);
    CHECK(closing.try_commit(ready, 100) == Cell::Commit::Retired);
    auto pending = parallel.snapshot();
    parallel.retire();
    parallel.retire();
    CHECK(parallel.try_commit(pending, 8) == Cell::Commit::Retired);
    bool rejected = false;
    try { (void)parallel.snapshot(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}
