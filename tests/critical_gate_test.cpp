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

    // Immediate try critical never registers a waiting ticket or waits
    // for another guest body to finish. Its result is scoped and unique.
    {
        CriticalGate try_gate;
        auto immediate = try_gate.try_request();
        check(immediate.has_value(), "uncontended try critical failed");
        check(try_gate.holds(*immediate), "try critical lacks ownership");
        check(!try_gate.try_request(), "contended try critical was admitted");
        check(try_gate.waiting() == 0, "try critical silently queued a waiter");
        const auto waiting = try_gate.request();
        check(waiting.admission == Admission::queued, "ordinary critical did not queue");
        check(!try_gate.try_request(), "try critical jumped FIFO queue");
        check(try_gate.release(*immediate) == waiting.token, "try path did not hand off");
        check(try_gate.holds(waiting.token), "queued ticket not handed off");
        check(!try_gate.release(waiting.token), "unexpected additional queued ticket");
        auto next = try_gate.try_request();
        check(next.has_value() && *next != *immediate, "try ticket reused");
        check(!try_gate.release(*immediate), "old try ticket released new owner");
        check(try_gate.holds(*next), "old ticket stole new try owner");
        (void)try_gate.release(*next);
    }

    // Retirement fences the *next* admission for old actor generations.
    // The scheduler must physically quiesce a running body before retiring.
    {
        CriticalGate old_generation;
        const auto held = old_generation.request();
        const auto queued = old_generation.request();
        check(old_generation.waiting() == 1, "retirement setup failed");
        old_generation.retire();
        check(old_generation.retired(), "critical gate did not retire");
        check(old_generation.waiting() == 0, "retirement leaked queued ticket");
        check(!old_generation.holds(held.token), "retired actor still owns gate");
        check(!old_generation.holds(queued.token), "retired queued ticket admitted");
        check(!old_generation.release(held.token), "retired owner released a successor");
        check(!old_generation.release(queued.token), "retired waiter released a successor");
        check(!old_generation.try_request(), "retired try critical was admitted");
        bool rejected = false;
        try { (void)old_generation.request(); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "retired gate accepted new invocation");
        old_generation.retire();
        CriticalGate replacement;
        const auto fresh_generation = replacement.request();
        check(fresh_generation.admission == Admission::acquired,
              "fresh generation cannot acquire a distinct critical gate");
        check(!replacement.holds(held.token), "stale generation held replacement gate");
        (void)replacement.release(fresh_generation.token);
    }

    (void)first.release(fresh.token);
    (void)second.release(independent.token);
    return 0;
}
