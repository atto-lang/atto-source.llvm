#include "oreslang/runtime/DbWalShardGrantRegistry.hpp"

#include <cassert>
#include <cstdint>
#include <thread>
#include <atomic>
#include <type_traits>

using oreslang::runtime::DbWalOwnerRetirementGate;
using Registry = oreslang::runtime::DbWalShardGrantRegistry<2>;
using File = Registry::FileIdentity;

static_assert(!std::is_copy_constructible_v<Registry::Grant>);
static_assert(!std::is_copy_assignable_v<Registry::Grant>);
static_assert(!std::is_move_assignable_v<Registry::Grant>);

int main() {
    Registry registry(55);
    Registry foreign(66);
    DbWalOwnerRetirementGate a(101, 7), b(102, 8), c(103, 9);
    const File fileA{11, 1001}, fileB{11, 1002};
    assert(!registry.issue(56, 1, 1, 101, 7, fileA, a));
    assert(!registry.issue(55, 0, 1, 101, 7, fileA, a));
    assert(!registry.issue(55, 1, 1, 101, 8, fileA, a));
    assert(!registry.issue(55, 1, 1, 101, 7, File{}, a));

    auto first = registry.issue(55, 1, 1, 101, 7, fileA, a);
    assert(first && registry.outstanding() == 1);
    assert(registry.can_write(*first, 55, 1, 1, 101, 7, fileA));
    assert(!registry.can_write(*first, 55, 2, 1, 101, 7, fileA));
    assert(!registry.can_write(*first, 55, 1, 1, 102, 7, fileA));
    assert(!registry.can_write(*first, 55, 1, 1, 101, 8, fileA));
    assert(!registry.can_write(*first, 55, 1, 1, 101, 7, fileB));
    assert(!foreign.can_write(*first, 55, 1, 1, 101, 7, fileA));
    assert(!registry.issue(55, 1, 1, 102, 8, fileB, b));
    assert(!registry.issue(55, 1, 2, 102, 8, fileA, b));

    auto second = registry.issue(55, 1, 2, 102, 8, fileB, b);
    assert(second && registry.outstanding() == 2);
    assert(!registry.issue(55, 2, 3, 103, 9, File{11, 1003}, c));

    assert(registry.begin_write(*first, 55, 1, 1, 101, 7, fileA));
    assert(!registry.begin_write(*first, 55, 1, 1, 101, 7, fileA));
    assert(!registry.begin_write(*first, 55, 2, 1, 101, 7, fileA));
    assert(!registry.begin_write(*first, 55, 1, 1, 101, 7, fileB));
    assert(!registry.begin_write(*first, 56, 1, 1, 101, 7, fileA));
    assert(!registry.drain(*first, 55, 101, 8)); // stale incarnation
    assert(!registry.drain(*first, 55, 101, 7)); // in flight
    assert(!registry.can_write(*first, 55, 1, 1, 101, 7, fileA));
    assert(!registry.begin_close(*first, 55, 101, 7));
    assert(!registry.issue(55, 1, 1, 103, 9, File{11, 1003}, c));
    assert(!registry.physical_write_retired(*first, 55, 101, 8));
    assert(registry.physical_write_retired(*first, 55, 101, 7));
    assert(!registry.physical_write_retired(*first, 55, 101, 7));
    assert(registry.drain(*first, 55, 101, 7));
    assert(registry.begin_close(*first, 55, 101, 7));
    assert(!registry.begin_close(*first, 55, 101, 7));
    assert(!registry.retire_after_physical_close(*first, a));
    assert(!registry.retire_after_physical_close(*first, b));
    a.physical_close_retired(101, 7);
    assert(registry.retire_after_physical_close(*first, a));
    assert(!registry.retire_after_physical_close(*first, a));
    assert(registry.outstanding() == 1);
    assert(!registry.can_write(*first, 55, 1, 1, 101, 7, fileA));

    // Reclaiming the slot must not resurrect a stale grant ticket.
    DbWalOwnerRetirementGate replacement(103, 9);
    auto reused = registry.issue(55, 1, 1, 103, 9, fileA, replacement);
    assert(reused && registry.outstanding() == 2);
    assert(!registry.can_write(*first, 55, 1, 1, 103, 9, fileA));
    assert(registry.can_write(*reused, 55, 1, 1, 103, 9, fileA));

    std::atomic<bool> start{false};
    std::atomic<unsigned> admitted{0};
    std::thread drain([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        (void)registry.drain(*reused, 55, 103, 9);
    });
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (registry.begin_write(*reused, 55, 1, 1, 103, 9, fileA))
            admitted.fetch_add(1, std::memory_order_relaxed);
    });
    start.store(true, std::memory_order_release);
    drain.join(); writer.join();
    assert(admitted.load() <= 1);
    assert(!registry.begin_write(*reused, 55, 1, 1, 103, 9, fileA));
    if (admitted.load()) {
        assert(!registry.begin_close(*reused, 55, 103, 9));
        assert(registry.physical_write_retired(*reused, 55, 103, 9));
    }
    assert(registry.drain(*reused, 55, 103, 9));
    assert(!registry.can_write(*reused, 55, 1, 1, 103, 9, fileA));
    registry.seal();
    assert(!registry.can_write(*second, 55, 1, 2, 102, 8, fileB));
    assert(!registry.issue(55, 2, 3, 103, 9, File{11, 1003}, c));
    assert(registry.outstanding() == 2); // no premature release on seal
    return 0;
}
