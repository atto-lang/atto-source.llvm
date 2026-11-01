#include "oreslang/runtime/LogicalMutex.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using oreslang::runtime::CriticalGate;
using oreslang::runtime::LogicalMutex;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using Admission = CriticalGate::Admission;
    LogicalMutex<int> mutex(0);
    auto first = mutex.request();
    auto second = mutex.request();
    auto third = mutex.request();
    check(first.admission == Admission::acquired, "first caller should own mutex");
    check(second.admission == Admission::queued, "second caller must queue");
    check(third.admission == Admission::queued, "third caller must queue");
    check(mutex.waiting() == 2, "mutex pending queue count incorrect");

    mutex.with_owner(first.token, [](int& value) { value += 10; });
    bool rejected = false;
    try {
        mutex.with_owner(second.token, [](int& value) { value = 999; });
    } catch (const std::logic_error&) {
        rejected = true;
    }
    check(rejected, "unadmitted waiter mutated protected state");

    // Cancelling an invocation is equivalent to retracting its queued ticket.
    check(!mutex.release(second.token), "cancelled waiter got permit");
    check(mutex.waiting() == 1, "cancelled waiter retained queue capacity");

    // An invocation can resume on another physical carrier without changing
    // the logical owner. Release hands off the oldest remaining ticket.
    std::atomic<bool> migrated{false};
    std::thread carrier([&] {
        migrated.store(mutex.owns(first.token), std::memory_order_release);
    });
    carrier.join();
    check(migrated.load(std::memory_order_acquire), "owner tied to thread");

    check(mutex.release(first.token) == third.token, "FIFO handoff failed");
    check(!mutex.owns(first.token), "stale owner retained capability");
    check(mutex.with_owner(third.token, [](int& value) {
        value += 2;
        return value;
    }) == 12, "admitted waiter did not receive shared payload");
    check(!mutex.release(third.token), "unexpected queued waiter");
    check(mutex.waiting() == 0, "mutex queue did not drain");

    auto next = mutex.request();
    check(next.admission == Admission::acquired, "mutex not released");
    check(next.token != first.token, "logical token reused");
    check(!mutex.release(first.token), "stale token evicted successor");
    check(mutex.with_owner(next.token, [](int& value) { return value; }) == 12,
          "state changed after cancelled/stale waiter");
    check(!mutex.release(next.token), "unexpected successor");

    // Stress release-vs-callback on a different physical carrier. A permit
    // must remain with the original logical owner until the callback unwinds.
    for (int round = 0; round < 1500; ++round) {
        auto executing = mutex.request();
        auto successor = mutex.request();
        std::atomic<bool> entered{false}, finish{false};
        std::thread carrier([&] {
            mutex.with_owner(executing.token, [&](int& value) {
                entered.store(true, std::memory_order_release);
                while (!finish.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ++value;
            });
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        bool denied = false;
        try { (void) mutex.release(executing.token); }
        catch (const std::logic_error&) { denied = true; }
        check(denied, "early logical permit release must be rejected");
        check(!mutex.owns(successor.token), "second caller admitted before turn ended");
        denied = false;
        try { mutex.with_owner(executing.token, [](int&) {}); }
        catch (const std::logic_error&) { denied = true; }
        check(denied, "overlapping protected callback was admitted");
        finish.store(true, std::memory_order_release);
        carrier.join();
        check(mutex.release(executing.token) == successor.token,
              "handoff lost after active callback finished");
        check(!mutex.release(successor.token), "unexpected queued task");
    }
    auto unwinding = mutex.request();
    try {
        mutex.with_owner(unwinding.token, [](int&) { throw std::runtime_error("guest failure"); });
    } catch (const std::runtime_error&) {}
    check(!mutex.release(unwinding.token), "exception must retire callback lease");
    {
        LogicalMutex<int> protectedState(10);
        auto owner = protectedState.try_request();
        check(owner.has_value(), "idle logical mutex try_request failed");
        check(!protectedState.try_request().has_value(), "busy logical mutex try_request succeeded");
        auto pending = protectedState.request();
        check(pending.admission == Admission::queued, "logical mutex pending slot absent");
        check(protectedState.retire() == 1, "logical mutex failed to revoke waiter");
        check(!protectedState.quiescent(), "live logical callback marked quiescent");
        bool deniedAfterRetirement = false;
        try {
            protectedState.with_owner(*owner, [](int& value) { value += 5; });
        } catch (const std::logic_error&) {
            deniedAfterRetirement = true;
        }
        check(deniedAfterRetirement, "retired owner began new protected callback");
        check(!protectedState.release(pending.token), "revoked logical ticket released owner");
        check(!protectedState.release(*owner), "retired logical mutex handed off owner");
        check(protectedState.quiescent(), "logical mutex not quiescent after callback retirement");
        bool reject = false;
        try { (void)protectedState.request(); } catch (const std::logic_error&) { reject = true; }
        check(reject, "retired logical mutex permitted new admission");
    }

    // Race retirement against an executing native protected callback.
    // Retirement must not reclaim active state or admit another callback.
    for (int i = 0; i < 128; ++i) {
        LogicalMutex<int> gate(0);
        auto owner = gate.try_request();
        check(owner.has_value(), "race setup missing owner");
        std::atomic<bool> inside{false}, finish{false};
        std::thread worker([&] {
            gate.with_owner(*owner, [&](int& value) {
                inside.store(true, std::memory_order_release);
                while (!finish.load(std::memory_order_acquire)) std::this_thread::yield();
                ++value;
            });
        });
        while (!inside.load(std::memory_order_acquire)) std::this_thread::yield();
        check(gate.retire() == 0, "retirement with active owner revoked callback");
        check(!gate.quiescent(), "retirement reclaimed an active protected body");
        finish.store(true, std::memory_order_release);
        worker.join();
        check(!gate.release(*owner), "retired callback handed off owner");
        check(gate.quiescent(), "retired callback failed to quiesce");
    }
    return 0;
}
