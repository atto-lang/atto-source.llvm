#include "oreslang/runtime/DbWalOwnerRetirementGate.hpp"
#include <cassert>
#include <atomic>
#include <thread>

using oreslang::runtime::DbWalOwnerRetirementGate;
using Phase = DbWalOwnerRetirementGate::Phase;

int main() {
    DbWalOwnerRetirementGate gate(99, 7);
    assert(!gate.admit(98, 7));
    assert(!gate.admit(99, 8));
    assert(gate.admit(99, 7));
    assert(!gate.admit(99, 7));
    assert(!gate.request_drain(99, 8));
    assert(gate.phase() == Phase::open);
    assert(!gate.request_drain(99, 7));
    assert(gate.phase() == Phase::draining);
    assert(!gate.admit(99, 7));
    assert(!gate.begin_close(99, 7));
    bool stale = false;
    try { gate.physical_write_retired(99, 8); }
    catch (const std::logic_error&) { stale = true; }
    assert(stale && gate.active());
    gate.physical_write_retired(99, 7);
    assert(gate.request_drain(99, 7));
    assert(gate.begin_close(99, 7));
    assert(!gate.begin_close(99, 7));
    assert(!gate.request_drain(99, 7));
    stale = false;
    try { gate.physical_close_retired(99, 8); }
    catch (const std::logic_error&) { stale = true; }
    assert(stale);
    gate.physical_close_retired(99, 7);
    assert(gate.phase() == Phase::closed);
    assert(!gate.admit(99, 7));
    assert(!gate.request_drain(99, 7));
    assert(!gate.begin_close(99, 7));
    stale = false;
    try { gate.physical_close_retired(99, 7); }
    catch (const std::logic_error&) { stale = true; }
    assert(stale);

    // Real threading race: both orderings must prevent post-drain admission.
    for (unsigned i = 0; i < 400; ++i) {
        DbWalOwnerRetirementGate racing(42, 123);
        assert(racing.admit(42, 123));
        std::atomic<bool> start{false};
        std::thread drainer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)racing.request_drain(42, 123);
        });
        std::thread worker([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            racing.physical_write_retired(42, 123);
        });
        start.store(true, std::memory_order_release);
        drainer.join(); worker.join();
        assert(!racing.admit(42, 123));
        assert(racing.request_drain(42, 123));
        assert(racing.begin_close(42, 123));
        assert(!racing.begin_close(42, 123));
        racing.physical_close_retired(42, 123);
        assert(racing.phase() == Phase::closed);
    }

    // A queued callback that never physically starts cannot release its slot
    // just because the logical Future is cancelled.
    DbWalOwnerRetirementGate never_started(5, 5);
    assert(never_started.admit(5, 5));
    assert(!never_started.request_drain(5, 5));
    assert(!never_started.begin_close(5, 5));
    assert(never_started.active());
    return 0;
}
