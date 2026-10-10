#include "oreslang/runtime/CriticalGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::CriticalGate;
static void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

int main() {
    CriticalGate first, second;
    check(first.admit(1) == CriticalGate::Admission::acquired, "first caller not admitted");
    check(first.admit(2) == CriticalGate::Admission::queued, "second caller should queue");
    check(first.admit(3) == CriticalGate::Admission::queued, "third caller should queue");
    check(second.admit(1) == CriticalGate::Admission::acquired, "instance gates not independent");

    std::atomic<bool> observed{false};
    std::thread migrated([&] {
        observed.store(first.holds(1), std::memory_order_release);
        check(first.release(1) == 2, "FIFO handoff after carrier migration failed");
    });
    migrated.join();
    check(observed.load(std::memory_order_acquire), "logical lock tied to thread identity");
    check(first.holds(2), "second token not granted");
    check(first.release(3) == 0, "queued cancellation should not release owner");
    check(first.release(2) == 0, "cancelled waiter should not receive handoff");
    check(first.waiting() == 0, "critical queue not drained");

    check(first.admit(4) == CriticalGate::Admission::acquired, "gate remained held");
    bool threw = false;
    try { first.admit(4); } catch (const std::logic_error&) { threw = true; }
    check(threw, "recursive ticket acquisition accepted");
    check(first.release(4) == 0, "unexpected waiter");

    check(first.admit(5) == CriticalGate::Admission::acquired, "not reacquired");
    for (std::size_t i = 0; i < CriticalGate::max_waiters; ++i) {
        check(first.admit(6 + i) == CriticalGate::Admission::queued, "queue admission failed");
    }
    threw = false;
    try { first.admit(9'999'999); } catch (const std::overflow_error&) { threw = true; }
    check(threw, "bounded queue admitted overflow");
    for (std::size_t i = 0; i < CriticalGate::max_waiters; ++i) first.release(6 + i);
    check(first.waiting() == 0, "queue cancellation leaked tickets");
    first.release(5);
    return 0;
}
