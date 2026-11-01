#include "oreslang/runtime/CriticalGate.hpp"
#include "oreslang/runtime/LogicalMutex.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::CriticalGate;
using oreslang::runtime::LogicalMutex;

static void check(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}

int main() {
    using Admission = CriticalGate::Admission;
    // Try-critical must never queue, block a scheduler, or jump a FIFO waiter.
    CriticalGate gate;
    const auto initial = gate.try_request();
    check(static_cast<bool>(initial), "uncontended try request did not acquire");
    check(gate.holds(initial), "try request did not own permit");
    check(!gate.try_request(), "recursive try request acquired");
    check(gate.waiting() == 0, "try request enqueued on contention");
    const auto waiting = gate.request();
    check(waiting.admission == Admission::queued, "normal contender did not queue");
    for (int i = 0; i < 1000; ++i) {
        check(!gate.try_request(), "try request jumped queued waiter");
        check(gate.waiting() == 1, "try request changed FIFO queue size");
    }
    check(gate.release(initial) == waiting.token, "release did not grant FIFO waiter");
    check(!gate.try_request(), "try request stole handed-off ticket");
    check(gate.holds(waiting.token), "FIFO waiter was not admitted");
    check(!gate.release(waiting.token), "extra phantom waiter");
    check(!gate.holds(initial), "stale try token remains owner");
    const auto newer = gate.try_request();
    check(newer && newer != initial, "try token was reused");
    check(!gate.release(initial), "stale release evicted new try owner");
    check(gate.holds(newer), "old ticket evicted new owner");

    // No thread affinity: scheduler may hand a logical admission to another
    // physical carrier. A losing simultaneous try only observes contention.
    std::atomic<int> wins{0};
    std::thread remote([&] {
        if (gate.try_request()) ++wins;
    });
    remote.join();
    check(wins == 0, "cross-thread try stole live ticket");
    check(!gate.release(newer), "unexpected token after try owner release");
    std::thread nextCarrier([&] {
        auto acquired = gate.try_request();
        if (acquired && gate.holds(acquired)) {
            ++wins;
            check(!gate.release(acquired), "cross-carrier release had phantom waiter");
        }
    });
    nextCarrier.join();
    check(wins == 1, "cross-carrier try/request/release failed");

    // Independent critical scopes may be acquired independently.
    CriticalGate another;
    const auto other = another.try_request();
    check(other && another.holds(other), "independent guard scope stalled");
    check(!gate.holds(other), "foreign scope token accepted");
    check(!another.release(other), "foreign queue affected");

    LogicalMutex<std::int64_t> typed(40);
    const auto owner = typed.try_request();
    check(owner && typed.owns(owner), "typed mutex try request denied");
    check(typed.with_owner(owner, [](std::int64_t& value) {
        value += 2;
        return value;
    }) == 42, "typed guard body did not execute");
    const auto queued = typed.request();
    check(queued.admission == Admission::queued, "typed contended ticket not queued");
    check(!typed.try_request(), "typed try jumped FIFO waiter");
    check(typed.waiting() == 1, "typed try added waiter");
    check(typed.release(owner) == queued.token, "typed queue handoff broken");
    check(typed.with_owner(queued.token, [](std::int64_t& value) {
        return value;
    }) == 42, "typed handoff changed protected state");
    check(!typed.release(queued.token), "typed mutex has phantom waiter");

    // The typed native substrate is NOT an Oreslang guest source ABI or an
    // actor-turn reservation. Only compiler+guest conformance may prove those.
    return 0;
}
