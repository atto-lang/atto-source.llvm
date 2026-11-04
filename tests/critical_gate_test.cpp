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
        const auto retirement = old_generation.retire();
        check(retirement.revoked_owner == held.token,
              "retirement lost exact owning actor ticket");
        check(retirement.cancelled_waiters.size() == 1
                      && retirement.cancelled_waiters.front() == queued.token,
              "retirement lost FIFO queued cancellation receipt");
        check(old_generation.retired(), "critical gate did not retire");
        check(old_generation.waiting() == 0, "retirement leaked queued ticket");
        check(!old_generation.holds(held.token), "retired actor still owns gate");
        check(!old_generation.holds(queued.token), "retired queued ticket admitted");
        check(!old_generation.quiescent(),
              "retired generation became quiescent before physical owner unwind");
        check(!old_generation.retire_if_quiescent(),
              "retired generation released live owner without unwind");
        check(!old_generation.release(held.token), "retired owner released a successor");
        check(old_generation.quiescent(),
              "retired generation did not quiesce after owner unwind");
        check(old_generation.retire_if_quiescent(),
              "retired quiescent generation was not idempotently fenced");
        check(!old_generation.release(queued.token), "retired waiter released a successor");
        check(!old_generation.try_request(), "retired try critical was admitted");
        bool rejected = false;
        try { (void)old_generation.request(); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "retired gate accepted new invocation");
        const auto repeated = old_generation.retire();
        check(!repeated.revoked_owner && repeated.cancelled_waiters.empty(),
              "repeated retirement delivered duplicate cancellation receipts");
        CriticalGate replacement;
        const auto fresh_generation = replacement.request();
        check(fresh_generation.admission == Admission::acquired,
              "fresh generation cannot acquire a distinct critical gate");
        check(!replacement.holds(held.token), "stale generation held replacement gate");
        (void)replacement.release(fresh_generation.token);
    }

    // Multiple outstanding waiters must be returned in original FIFO order,
    // not discarded or reordered during retirement. This is a host contract;
    // delivering the receipts on a scheduler control lane remains separate.
    {
        CriticalGate gate;
        const auto owner = gate.request();
        const auto firstWaiter = gate.request();
        const auto secondWaiter = gate.request();
        const auto thirdWaiter = gate.request();
        (void)gate.release(secondWaiter.token); // cancelled before STOP
        const auto receipt = gate.retire();
        check(receipt.revoked_owner == owner.token,
              "retirement did not identify active owner");
        check(receipt.cancelled_waiters.size() == 2,
              "retirement duplicated or omitted queued tickets");
        check(receipt.cancelled_waiters[0] == firstWaiter.token
                  && receipt.cancelled_waiters[1] == thirdWaiter.token,
              "retirement cancellation order differs from FIFO");
        check(!gate.holds(owner.token) && gate.waiting() == 0,
              "retired gate retained executable ticket authority");
        check(!gate.quiescent(), "retirement prematurely lost physical owner");
        check(!gate.retire_if_quiescent(), "retirement bypassed owner cleanup");
        check(!gate.release(firstWaiter.token),
              "cancelled waiter re-entered retired generation");
        check(!gate.release(owner.token),
              "revoked owner handed off after retirement");
        check(gate.quiescent(), "retired physical owner failed to unwind");
        const auto duplicate = gate.retire();
        check(!duplicate.revoked_owner && duplicate.cancelled_waiters.empty(),
              "stale retirement duplicated tickets");
    }

    // A release-versus-STOP race may return a promoted ticket to the
    // scheduler, but retirement must still produce exactly one terminal
    // disposition for every waiting ticket. No stale promotion may execute
    // in the retired actor incarnation, regardless of thread ordering.
    for (int iteration = 0; iteration < 256; ++iteration) {
        CriticalGate gate;
        const auto owner = gate.request();
        const auto waitingA = gate.request();
        const auto waitingB = gate.request();
        CriticalGate::Token handoff;
        CriticalGate::Retirement receipt;
        std::thread releaser([&] { handoff = gate.release(owner.token); });
        std::thread retiring([&] { receipt = gate.retire(); });
        releaser.join();
        retiring.join();

        check(gate.retired(), "release-vs-retirement lost closure");
        check(gate.waiting() == 0, "release-vs-retirement retained queue");
        check(!gate.holds(owner.token) && !gate.holds(waitingA.token)
                  && !gate.holds(waitingB.token),
              "release-vs-retirement retained executable ticket");
        check(!handoff || handoff == waitingA.token,
              "handoff skipped the first FIFO waiter");

        const auto disposition = [&](CriticalGate::Token ticket) {
            int count = receipt.revoked_owner == ticket ? 1 : 0;
            for (const auto queued : receipt.cancelled_waiters) {
                if (queued == ticket) ++count;
            }
            return count;
        };
        check(disposition(waitingA.token) == 1
                  && disposition(waitingB.token) == 1,
              "waiting ticket lost or duplicated on concurrent STOP");
        check(receipt.revoked_owner == owner.token
                  || receipt.revoked_owner == waitingA.token,
              "retirement did not record the last live owner");
        check(!gate.release(waitingA.token) && !gate.release(waitingB.token),
              "old retired waiter was readmitted");
        check(!gate.release(receipt.revoked_owner),
              "retired owner handed off a new invocation");
        check(gate.quiescent(), "retirement race lost physical owner unwind");
        check(gate.retire_if_quiescent(), "retired drained gate not idempotent");
    }

    // Java/LLVM host-gate parity: retirement of an EMPTY gate must be atomic
    // with request admission. Never observe quiescence, unlock, then close.
    {
        CriticalGate gate;
        check(gate.quiescent(), "fresh gate was not quiescent");
        check(gate.retire_if_quiescent(), "empty gate retirement failed");
        check(gate.retired(), "empty retirement did not close generation");
        check(gate.retire_if_quiescent(), "duplicate retirement was not idempotent");
        check(!gate.try_request(), "retired gate allowed immediate ticket");
        bool rejected = false;
        try { (void)gate.request(); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "retired gate admitted a queued ticket");
    }
    {
        CriticalGate gate;
        const auto owner = gate.request();
        const auto pending = gate.request();
        check(!gate.quiescent(), "live owner and waiter looked quiescent");
        check(!gate.retire_if_quiescent(), "live gate retired as empty");
        check(gate.holds(owner.token), "failed retirement revoked active owner");
        check(gate.release(owner.token) == pending.token,
              "failed retirement disrupted FIFO handoff");
        check(!gate.retire_if_quiescent(), "promoted owner retired as empty");
        (void)gate.release(pending.token);
        check(gate.quiescent(), "drained gate did not become quiescent");
        check(gate.retire_if_quiescent(), "drained gate did not retire atomically");
    }
    for (int iteration = 0; iteration < 256; ++iteration) {
        CriticalGate gate;
        std::atomic<bool> start{false};
        std::atomic<bool> retired{false};
        std::atomic<bool> admitted{false};
        std::atomic<bool> unexpected{false};
        CriticalGate::Token acquired;
        std::thread requester([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                acquired = gate.request().token;
                admitted.store(true, std::memory_order_release);
            } catch (const std::logic_error&) {
                if (!gate.retired()) unexpected.store(true, std::memory_order_release);
            } catch (...) {
                unexpected.store(true, std::memory_order_release);
            }
        });
        std::thread stopper([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            retired.store(gate.retire_if_quiescent(), std::memory_order_release);
        });
        start.store(true, std::memory_order_release);
        requester.join();
        stopper.join();
        check(!unexpected.load(), "request/retire race raised unexpected error");
        if (retired.load()) {
            check(!admitted.load(), "request admitted after atomic empty retirement");
            check(gate.retired(), "successful retirement not fenced");
        } else {
            check(admitted.load(), "retirement failed without live ticket");
            check(gate.holds(acquired), "winning request lost ticket");
            (void)gate.release(acquired);
            check(gate.retire_if_quiescent(), "drained race gate failed retirement");
        }
        check(gate.retired() && gate.quiescent(),
              "request/retire race left a live retired generation");
    }

    (void)first.release(fresh.token);
    (void)second.release(independent.token);
    return 0;
}
