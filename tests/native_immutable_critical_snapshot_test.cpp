#include "oreslang/runtime/CriticalImmutableSnapshotRoot.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Cell = oreslang::runtime::CriticalImmutableSnapshotRoot<32, 4>;
using Result = Cell::Result;

[[noreturn]] static void fail(const char* what) {
    std::fprintf(stderr, "FAIL critical immutable host test: %s\n", what);
    std::abort();
}
static void check(bool yes, const char* why) { if (!yes) fail(why); }

static void single_snapshot_and_reclamation() {
    Cell cell;
    Cell::Lease first, second;
    check(cell.try_snapshot(first) == Result::Ok, "first snapshot");
    check(cell.try_snapshot(second) == Result::Ok, "second snapshot");
    Cell::Candidate candidate;
    check(cell.try_stage(first, 0, 7, candidate) == Result::Ok, "stage copy outside critical");
    check(cell.try_publish(first, candidate) == Result::Ok, "publish versioned root");
    std::int64_t value = -1;
    check(cell.read(first, 0, value) == Result::Ok && value == 0,
          "old immutable snapshot must remain readable");
    check(cell.retired_pinned() == 1, "old snapshot is retained by readers");
    Cell::Lease fresh;
    check(cell.try_snapshot(fresh) == Result::Ok && fresh.revision() == 1,
          "atomic root and revision snapshot");
    check(cell.read(fresh, 0, value) == Result::Ok && value == 7,
          "new snapshot value");
    check(cell.try_release(first) == Result::Ok, "first release");
    check(cell.retired_pinned() == 1, "second lease retains old root");
    check(cell.try_release(second) == Result::Ok, "last release");
    check(cell.retired_pinned() == 0, "last reader reclaims old root");
    check(cell.try_release(first) == Result::Stale, "double release rejected");
    check(cell.try_release(fresh) == Result::Ok, "fresh release");
}

static void conflict_rejected_without_mutation() {
    Cell cell;
    Cell::Lease a, b;
    check(cell.try_snapshot(a) == Result::Ok &&
          cell.try_snapshot(b) == Result::Ok, "competing snapshots");
    Cell::Candidate first, second;
    check(cell.try_stage(a, 0, 31, first) == Result::Ok, "stage first");
    check(cell.try_stage(b, 0, 67, second) == Result::Ok, "stage second");
    check(cell.try_publish(a, first) == Result::Ok, "first wins");
    check(cell.try_publish(b, second) == Result::Conflict, "stale update denied");
    check(cell.try_discard(second) == Result::Ok, "stale candidate reclaimed");
    Cell::Lease current;
    check(cell.try_snapshot(current) == Result::Ok, "current snapshot");
    std::int64_t val = 0;
    check(current.revision() == 1 &&
          cell.read(current, 0, val) == Result::Ok && val == 31,
          "rejected version does not mutate published value");
    check(cell.try_release(a) == Result::Ok && cell.try_release(b) == Result::Ok &&
          cell.try_release(current) == Result::Ok, "leases released");
}

static void capacity_denied_preserves_live_root() {
    using Tiny = oreslang::runtime::CriticalImmutableSnapshotRoot<2, 1>;
    Tiny cell;
    Tiny::Lease basis;
    check(cell.try_snapshot(basis) == Tiny::Result::Ok, "small snapshot");
    Tiny::Candidate one, two;
    check(cell.try_stage(basis, 0, 19, one) == Tiny::Result::Ok, "small staging");
    check(cell.try_stage(basis, 0, 20, two) == Tiny::Result::Capacity,
          "capacity fails without root damage");
    check(cell.try_discard(one) == Tiny::Result::Ok, "discard returns slot");
    check(cell.try_stage(basis, 0, 20, two) == Tiny::Result::Ok,
          "released slot admits candidate");
    check(cell.try_publish(basis, two) == Tiny::Result::Ok,
          "small root atomically publishes");
    check(cell.try_release(basis) == Tiny::Result::Ok, "small lease release");
}

static void concurrent_writers() {
    Cell cell;
    constexpr int workers = 6;
    constexpr int increments = 700;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int tid = 0; tid < workers; ++tid) {
        threads.emplace_back([&] {
            for (int round = 0; round < increments; ++round) {
                for (;;) {
                    Cell::Lease basis;
                    auto admitted = cell.try_snapshot(basis);
                    if (admitted == Result::Busy) { std::this_thread::yield(); continue; }
                    if (admitted != Result::Ok) { ++failures; return; }
                    std::int64_t current = -1;
                    if (cell.read(basis, 0, current) != Result::Ok) { ++failures; return; }
                    Cell::Candidate candidate;
                    for (;;) {
                        auto staged = cell.try_stage(basis, 0, current + 1, candidate);
                        if (staged == Result::Busy) { std::this_thread::yield(); continue; }
                        if (staged == Result::Capacity) {
                            // Release long-lived snapshot before allowing other
                            // writers/reader generations to reclaim old slots.
                            while (cell.try_release(basis) == Result::Busy)
                                std::this_thread::yield();
                            std::this_thread::yield();
                            break;
                        }
                        if (staged != Result::Ok) { ++failures; return; }
                        Result published;
                        do { published = cell.try_publish(basis, candidate);
                             if (published == Result::Busy) std::this_thread::yield();
                        } while (published == Result::Busy);
                        if (published == Result::Conflict) {
                            while (cell.try_discard(candidate) == Result::Busy)
                                std::this_thread::yield();
                        } else if (published != Result::Ok) { ++failures; return; }
                        while (cell.try_release(basis) == Result::Busy)
                            std::this_thread::yield();
                        if (published == Result::Ok) goto done_increment;
                        break;
                    }
                }
            done_increment: ;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(failures.load() == 0, "concurrent acquisition and publication failed");
    Cell::Lease last;
    while (cell.try_snapshot(last) == Result::Busy) std::this_thread::yield();
    std::int64_t value = -1;
    check(cell.read(last, 0, value) == Result::Ok &&
          value == workers * increments &&
          last.revision() == workers * increments,
          "all racing writers must publish exactly once with no lost updates");
    check(cell.try_release(last) == Result::Ok, "release final reader");
    std::printf("PASS native host immutable critical snapshot: %d contending writers, %d published revisions\n",
                workers, workers * increments);
}

int main() {
    single_snapshot_and_reclamation();
    conflict_rejected_without_mutation();
    capacity_denied_preserves_live_root();
    concurrent_writers();
    return 0;
}
