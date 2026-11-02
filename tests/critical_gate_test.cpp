#include "oreslang/runtime/CriticalGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::CriticalGate;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    using Admission = CriticalGate::Admission;
    CriticalGate first, second;
    const auto a = first.request();
    const auto b = first.request();
    const auto c = first.request();
    const auto independent = second.request();
    check(a.admission == Admission::acquired, "first caller not admitted");
    check(b.admission == Admission::queued, "second caller should queue");
    check(c.admission == Admission::queued, "third caller should queue");
    check(independent.admission == Admission::acquired, "scopes are not independent");

    std::atomic<bool> observed{false};
    std::atomic<bool> handedOff{false};
    std::thread migrated([&] {
        observed.store(first.holds(a.token), std::memory_order_release);
        handedOff.store(first.release(a.token) == b.token, std::memory_order_release);
    });
    migrated.join();
    check(observed.load(std::memory_order_acquire), "ticket tied to carrier thread");
    check(handedOff.load(std::memory_order_acquire), "FIFO handoff failed");
    check(first.holds(b.token), "second token not admitted");
    check(!first.release(c.token), "cancelled waiter received handoff");
    check(!first.release(b.token), "unexpected waiter after cancelled third");
    check(first.waiting() == 0, "critical queue not drained");
    check(!first.holds(a.token), "stale ticket still owns gate");

    // A stale completion can never release a later logical invocation.
    const auto fresh = first.request();
    check(fresh.admission == Admission::acquired, "could not reacquire");
    check(fresh.token != a.token, "ticket reused after completion");
    check(!first.release(a.token), "old token released a new invocation");
    check(first.holds(fresh.token), "stale release evicted fresh owner");

    // Tickets cannot be transferred between two critical scopes.
    bool threw = false;
    try {
        (void)second.release(fresh.token);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "foreign critical token release accepted");
    check(second.holds(independent.token), "foreign release modified other gate");

    const auto firstInQueue = first.request();
    for (std::size_t i = 1; i < CriticalGate::max_waiters; ++i) {
        const auto pending = first.request();
        check(pending.admission == Admission::queued, "queued admission failed");
    }
    check(first.waiting() == CriticalGate::max_waiters, "queue capacity not enforced");
    threw = false;
    try { (void)first.request(); } catch (const std::overflow_error&) { threw = true; }
    check(threw, "critical queue accepted overflow");

    // Cancellation of a queued ticket never steals ownership.
    check(!first.release(firstInQueue.token), "queued cancellation granted owner");
    check(first.holds(fresh.token), "queued cancellation released owner");
    const auto extra = first.request();
    check(extra.admission == Admission::queued, "capacity slot not returned");

    // Cancel the remaining oldest waiters, then hand off to extra.
    // Tickets are issued monotonically but must still be cancelled through
    // their opaque handles; exercise a separate short queue for full handoff.
    {
        CriticalGate small;
        const auto root = small.request();
        const auto cancelled = small.request();
        const auto next = small.request();
        check(!small.release(cancelled.token), "cancelled queued ticket acquired");
        check(small.release(root.token) == next.token, "cancelled waiter starved successor");
        check(!small.release(next.token), "unexpected queued successor");
        check(!small.release(root.token), "duplicate stale release changed gate");
    }

    // Cancellation racing against release may win either way. Scheduler must
    // recheck holds() on its turn; a cancelled ticket must never run.
    for (int i = 0; i < 512; ++i) {
        CriticalGate gate;
        const auto owner = gate.request();
        const auto candidate = gate.request();
        const auto survivor = gate.request();
        CriticalGate::Token observedHandoff;
        std::thread releaser([&] { observedHandoff = gate.release(owner.token); });
        std::thread canceller([&] { (void)gate.release(candidate.token); });
        releaser.join();
        canceller.join();
        check(!gate.holds(candidate.token), "cancelled candidate remained owner");
        check(gate.holds(survivor.token), "handoff lost surviving waiter");
        check(!gate.release(survivor.token), "unexpected final successor");
        check(!gate.holds(survivor.token), "gate leaked after racing cancellation");
        (void)observedHandoff;
    }

    (void)first.release(fresh.token);
    (void)second.release(independent.token);

    // A nonqueueing try enter never grows a FIFO queue and never outruns
    // an already queued ticket. A successful fast path can be released.
    {
        CriticalGate gate;
        auto fast = gate.try_request();
        check(fast.has_value(), "idle try_request must acquire");
        check(!gate.try_request().has_value(), "busy try_request acquired owner");
        check(gate.waiting() == 0, "try_request enqueued a waiter");
        const auto queued = gate.request();
        check(queued.admission == Admission::queued, "expected queued waiter");
        check(!gate.try_request().has_value(), "try_request bypassed FIFO waiter");
        check(gate.release(*fast) == queued.token, "FIFO handoff lost");
        check(!gate.try_request().has_value(), "try_request displaced new owner");
        check(!gate.release(queued.token), "final try_request test leaked waiter");
        auto last = gate.try_request();
        check(last.has_value(), "gate did not become usable again");
        check(!gate.release(*last), "last try-enter unexpectedly handed off");
    }

    // Retiring an incarnation revokes its waiting tickets but never
    // releases or reclaims the active body before physical quiescence.
    {
        CriticalGate gate;
        auto active = gate.try_request();
        check(active.has_value(), "retirement setup has no owner");
        auto queuedA = gate.request();
        auto queuedB = gate.request();
        const auto revoked = gate.retire();
        check(revoked.cancelled_waiters.size() == 2,
              "retire did not return two cancellation receipts");
        check(revoked.cancelled_waiters[0] == queuedA.token
              && revoked.cancelled_waiters[1] == queuedB.token,
              "retirement receipts did not preserve FIFO order");
        check(revoked.owner == *active, "retirement omitted active owner identity");
        check(gate.retired(), "retire state absent");
        check(!gate.quiescent(), "active owner declared physically quiescent");
        check(gate.waiting() == 0, "retired queued tickets retained capacity");
        check(!gate.release(queuedA.token), "stale queued ticket released owner");
        check(!gate.release(queuedB.token), "second stale ticket released owner");
        check(gate.holds(*active), "active protected body prematurely revoked");
        bool reject = false;
        try { (void)gate.request(); } catch (const std::logic_error&) { reject = true; }
        check(reject, "retired gate admitted queued request");
        reject = false;
        try { (void)gate.try_request(); } catch (const std::logic_error&) { reject = true; }
        check(reject, "retired gate admitted try-request");
        const auto repeat = gate.retire();
        check(repeat.cancelled_waiters.empty() && !repeat.owner,
              "duplicate retirement issued duplicate cancellation receipts");
        check(!gate.release(*active), "retired owner was handed to a waiter");
        check(gate.quiescent(), "retired gate did not become quiescent");
        check(!gate.holds(*active), "stale active ticket retained owner after exit");
    }
    return 0;
}
