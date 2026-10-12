#include "oreslang/runtime/VersionedActorCell.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

int main() {
    using Cell = oreslang::VersionedActorCell;
    Cell cell(5);
    auto old = cell.snapshot();
    assert(old.value() == 5);
    assert(cell.try_commit(old, 6) == Cell::Commit::Committed);
    assert(cell.try_commit(old, 7) == Cell::Commit::Conflict);
    auto next = cell.snapshot();
    assert(next.value() == 6);
    assert(cell.try_commit(next, 5) == Cell::Commit::Committed); // value ABA
    assert(cell.try_commit(old, 8) == Cell::Commit::Conflict);
    Cell foreign(5);
    assert(foreign.try_commit(cell.snapshot(), 7) == Cell::Commit::Foreign);

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
    assert(winners == 1);

    Cell exhausted(0, static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()));
    assert(exhausted.try_commit(exhausted.snapshot(), 4) == Cell::Commit::Exhausted);
    auto pending = parallel.snapshot();
    parallel.retire();
    parallel.retire();
    assert(parallel.try_commit(pending, 8) == Cell::Commit::Retired);
    bool rejected = false;
    try { (void)parallel.snapshot(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
}
