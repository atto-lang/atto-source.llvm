#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"
#include <cassert>
#include <cstdint>
#include <limits>

using Queue = oreslang::runtime::realtime::HardRealtimeTimerQueue<2>;
using Status = Queue::Status;

// Deterministic arithmetic/admission oracle: unlike a wall-clock timing
// benchmark this checks 12k exact boundaries and near-UINT64_MAX cases under
// each native compiler/sanitizer build. It does NOT certify guest-start WCET.
static void sweep_dispatch_budget_invariants() {
    std::uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
    auto next = [&seed]() noexcept -> std::uint64_t {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        return seed * UINT64_C(2685821657736338717);
    };
    for (std::size_t i = 0; i < 12000; ++i) {
        const std::uint64_t deadline = (i % 7 == 0)
            ? UINT64_MAX - (next() % 512)
            : next() % 1000000;
        const std::uint64_t actor_id = 1 + next() % 1000;
        const std::uint64_t generation = 1 + next() % 1000;
        const std::uint64_t budget = (i % 19 == 0)
            ? UINT64_MAX : next() % 1024;
        const std::uint64_t reserve = (i % 23 == 0)
            ? UINT64_MAX : (i % 29 == 0 ? 0 : next() % 1024 + 1);
        const std::uint64_t wanted_lateness = (i % 11 == 0)
            ? 0 : (i % 13 == 0 ? budget : next() % 1024);
        const std::uint64_t lateness = wanted_lateness >
            UINT64_MAX - deadline ? UINT64_MAX - deadline : wanted_lateness;

        Queue timer;
        Queue::Handle ticket{};
        Queue::Expired expiry{};
        Queue::Start receipt{};
        assert(timer.arm_after(deadline, 0, actor_id, generation,
                               budget, ticket) == Status::Ok);
        assert(timer.poll_due(deadline, expiry) == Status::Ok);
        assert(expiry.handle.domain == ticket.domain);
        assert(expiry.handle.generation == ticket.generation);
        // A forged generation and far-future timestamp cannot poison the
        // monotonic watermark of the legitimate READY ticket.
        assert(timer.claim_start_with_dispatch_bound(
                   ticket, actor_id, generation + 1, UINT64_MAX,
                   reserve == 0 ? 1 : reserve, receipt) == Status::Stale);
        assert(receipt.claimed_at_ns == 0 && timer.outstanding() == 1);

        const auto observed = deadline + lateness; // Saturation above.
        const auto got = timer.claim_start_with_dispatch_bound(
            ticket, actor_id, generation, observed, reserve, receipt);
        if (reserve == 0) {
            assert(got == Status::Invalid);
            assert(timer.outstanding() == 1);
            assert(receipt.deadline_ns == 0);
            assert(timer.cancel(ticket) == Status::Ok);
        } else {
            const bool expected_miss = lateness > budget ||
                reserve > budget - lateness; // Short-circuit prevents underflow.
            assert(got == (expected_miss ? Status::DeadlineMissed : Status::Ok));
            assert(receipt.deadline_ns == deadline);
            assert(receipt.claimed_at_ns == observed);
            assert(receipt.lateness_ns == lateness);
            assert(receipt.maximum_lateness_ns == budget);
            assert(receipt.dispatch_bound_applied);
            assert(receipt.dispatch_bound_ns == reserve);
            assert(timer.outstanding() == 0);
            assert(timer.claim_start_with_dispatch_bound(
                       ticket, actor_id, generation, observed, reserve, receipt)
                   == Status::Stale);
            assert(receipt.deadline_ns == 0);
        }
        assert(timer.outstanding() == 0);
        // A new tenant can immediately reuse the physical slot without
        // inheriting an old ticket, actor generation or deadline authority.
        Queue::Handle next_tenant{};
        std::size_t revoked = 77;
        assert(timer.arm_after(observed, 0, actor_id + 10000,
                               generation + 1, 0, next_tenant) == Status::Ok);
        assert(next_tenant.domain == ticket.domain);
        assert(next_tenant.slot == ticket.slot);
        assert(next_tenant.generation != ticket.generation);
        assert(timer.cancel(ticket) == Status::Stale);
        assert(timer.revoke_actor(actor_id, generation, revoked) == Status::Ok);
        assert(revoked == 0 && timer.outstanding() == 1);
        assert(timer.cancel(next_tenant) == Status::Ok);
        assert(timer.outstanding() == 0);
    }
}

int main() {
    Queue q;
    Queue::Handle a{}, b{}, c{};
    Queue::Expired expiry{};
    Queue::Start start{};
    assert(q.capacity() == 2);
    assert(q.arm_after(100, 10, 7, 1, 1, a) == Status::Ok);
    assert(q.arm_after(101, 2, 8, 1, 1, b) == Status::Ok);
    assert(q.arm_after(101, 0, 7, 1, 1, c) == Status::Full);
    assert(q.poll_due(102, expiry) == Status::NotReady);
    assert(q.poll_due(103, expiry) == Status::Ok);
    assert(expiry.handle.domain == b.domain && expiry.handle.generation == b.generation);
    assert(expiry.actor_id == 8 && expiry.actor_generation == 1);
    assert(expiry.deadline_ns == 103 && expiry.observed_expiry_ns == 103);
    assert(q.outstanding() == 2);  // READY tickets still retain quota.
    assert(q.claim_start(b, 8, 2, 104, start) == Status::Stale);
    assert(q.claim_start(b, 8, 1, 104, start) == Status::Ok);
    assert(start.lateness_ns == 1);
    assert(start.deadline_ns == 103 && start.claimed_at_ns == 104);
    assert(start.maximum_lateness_ns == 1);
    assert(q.claim_start(b, 8, 1, 105, start) == Status::Stale);
    assert(q.poll_due(110, expiry) == Status::Ok);
    assert(q.cancel(a) == Status::Ok);  // READY callback still cancellable.
    assert(q.claim_start(a, 7, 1, 110, start) == Status::Stale);
    assert(q.outstanding() == 0);
    assert(q.arm_after(110, 0, 7, 2, 3, c) == Status::Ok);
    assert(c.slot == a.slot || c.slot == b.slot);
    assert(q.cancel(a) == Status::Stale);  // Slot ABA forbidden.
    assert(q.poll_due(110, expiry) == Status::Ok);
    assert(q.claim_start(c, 7, 2, 125, start) == Status::DeadlineMissed);
    assert(start.lateness_ns == 15 && q.outstanding() == 0);
    assert(q.poll_due(124, expiry) == Status::ClockBackwards);
    assert(q.arm_after(UINT64_MAX, 1, 7, 2, 1, c) == Status::Overflow);
    Queue::Handle z{};
    assert(q.arm_after(UINT64_MAX, 0, 7, 2, 1, z) == Status::Ok);
    assert(q.cancel(z) == Status::Ok);
    assert(q.arm_after(UINT64_MAX, 0, 0, 2, 1, z) == Status::Invalid);
    Queue q2;
    assert(q2.arm_after(0, 0, 9, 1, 1, z) == Status::Ok);
    const Queue::Handle q2_ticket = z;  // Preserve owner ticket before z is reused.
    assert(q.cancel(z) == Status::Stale);  // Cross-scheduler domain.
    q.close();
    assert(q.outstanding() == 0);
    assert(q.poll_due(UINT64_MAX, expiry) == Status::Closed);
    assert(q.arm_after(UINT64_MAX, 0, 7, 2, 1, z) == Status::Closed);
    assert(q2.cancel(z) == Status::Stale);  // Closed q cleared reused output.
    assert(q2.cancel(q2_ticket) == Status::Ok);

    // STOP of one actor generation retires READY and ARMED reservations,
    // leaving the replacement generation intact. Tie order is stable.
    Queue q3;
    Queue::Handle old0{}, old1{}, replacement{};
    std::uint64_t next_ns = 0;
    std::size_t revoked = 99;
    assert(q3.arm_after(1, 9, 44, 3, 1, old0) == Status::Ok);
    assert(q3.arm_after(1, 9, 44, 3, 1, old1) == Status::Ok);
    assert(q3.next_deadline_ns(next_ns) == Status::Ok && next_ns == 10);
    assert(q3.poll_due(10, expiry) == Status::Ok);
    assert(expiry.handle.slot == old0.slot);  // same deadline FIFO
    assert(q3.revoke_actor(44, 3, revoked) == Status::Ok && revoked == 2);
    assert(q3.claim_start(old0, 44, 3, 10, start) == Status::Stale);
    assert(q3.outstanding() == 0);
    assert(q3.next_deadline_ns(next_ns) == Status::NotReady);
    assert(q3.arm_after(10, 0, 44, 4, 1, replacement) == Status::Ok);
    assert(q3.revoke_actor(44, 3, revoked) == Status::Ok && revoked == 0);
    assert(q3.poll_due(10, expiry) == Status::Ok);
    assert(q3.claim_start(replacement, 44, 4, 10, start) == Status::Ok);

    // Timer domains must never alias between different template capacities:
    // old template-private mint_domain() counters used to collide here.
    oreslang::runtime::realtime::HardRealtimeTimerQueue<3> other_capacity;
    oreslang::runtime::realtime::HardRealtimeTimerQueue<3>::Handle other_h{};
    assert(other_capacity.arm_after(0, 0, 77, 1, 1, other_h)
        == oreslang::runtime::realtime::HardRealtimeTimerQueue<3>::Status::Ok);
    assert(q3.arm_after(10, 0, 78, 1, 1, replacement) == Status::Ok);
    assert(replacement.domain != other_h.domain);
    assert(q3.cancel(replacement) == Status::Ok);

    // Immutable zero-lateness admission: the dispatcher cannot retroactively
    // relax this timer's budget just because an actor carrier starts late.
    // Stale and wrong-generation claims cannot poison monotonic clock time.
    Queue strict;
    Queue::Handle strict_handle{}, forged{};
    Queue::Expired strict_due{};
    Queue::Start strict_start{};
    assert(strict.arm_after(100, 5, 42, 9, 0, strict_handle) == Status::Ok);
    assert(strict.poll_due(105, strict_due) == Status::Ok);
    forged = strict_handle;
    ++forged.generation;
    assert(strict.claim_start(forged, 42, 9, UINT64_MAX, strict_start)
           == Status::Stale);
    assert(strict.claim_start(strict_handle, 42, 10, UINT64_MAX, strict_start)
           == Status::Stale);
    assert(strict.claim_start(strict_handle, 42, 9, 106, strict_start)
           == Status::DeadlineMissed);
    assert(strict_start.deadline_ns == 105);
    assert(strict_start.claimed_at_ns == 106);
    assert(strict_start.maximum_lateness_ns == 0);
    assert(strict_start.lateness_ns == 1);
    assert(strict.outstanding() == 0);
    assert(strict.claim_start(strict_handle, 42, 9, 106, strict_start)
           == Status::Stale);

    // A deadline boundary is inclusive: lateness == admitted budget passes.
    Queue exact;
    Queue::Handle exact_handle{};
    assert(exact.arm_after(20, 5, 77, 2, 2, exact_handle) == Status::Ok);
    assert(exact.poll_due(25, strict_due) == Status::Ok);
    assert(exact.claim_start(exact_handle, 77, 2, 27, strict_start)
           == Status::Ok);
    assert(strict_start.lateness_ns == 2);
    assert(strict_start.maximum_lateness_ns == 2);

    // Rejected registrations are not clock observations. Previously a
    // full/overflowing arm at UINT64_MAX advanced the watermark and made
    // otherwise valid earlier polls/arms fail with ClockBackwards.
    Queue rejection;
    Queue::Handle pending{}, near_due{}, denied{};
    Queue::Expired rejection_due{};
    assert(rejection.arm_after(10, 20, 101, 1, 1, pending) == Status::Ok);
    assert(rejection.arm_after(UINT64_MAX, 1, 102, 1, 1, denied)
           == Status::Overflow);
    assert(denied.domain == 0 && rejection.outstanding() == 1);
    assert(rejection.poll_due(11, rejection_due) == Status::NotReady);
    assert(rejection.arm_after(11, 2, 102, 1, 1, near_due) == Status::Ok);
    assert(rejection.arm_after(UINT64_MAX, 0, 103, 1, 1, denied)
           == Status::Full);
    assert(denied.domain == 0 && rejection.outstanding() == 2);
    assert(rejection.poll_due(12, rejection_due) == Status::NotReady);
    assert(rejection.poll_due(13, rejection_due) == Status::Ok);
    assert(rejection_due.handle.domain == near_due.domain &&
           rejection_due.handle.generation == near_due.generation &&
           rejection_due.handle.slot == near_due.slot);
    assert(rejection.claim_start(near_due, 102, 1, 13, strict_start)
           == Status::Ok);
    assert(rejection.cancel(pending) == Status::Ok);
    assert(rejection.outstanding() == 0);
    // Failed operations fail closed even when the caller reuses output storage:
    // old tickets and start receipts cannot masquerade as new authority.
    Queue receipt;
    Queue::Handle reused{}, saved{};
    Queue::Expired reuse_due{};
    Queue::Start reuse_start{};
    std::uint64_t next_deadline = 12345;
    assert(receipt.arm_after(50, 1, 314, 1, 0, reused) == Status::Ok);
    saved = reused;
    assert(receipt.arm_after(50, 0, 0, 1, 0, reused) == Status::Invalid);
    assert(reused.domain == 0 && reused.generation == 0);
    assert(receipt.cancel(saved) == Status::Ok);
    assert(receipt.next_deadline_ns(next_deadline) == Status::NotReady);
    assert(next_deadline == 0);
    assert(receipt.arm_after(51, 0, 314, 1, 0, reused) == Status::Ok);
    assert(receipt.poll_due(51, reuse_due) == Status::Ok);
    assert(reuse_due.handle.domain == reused.domain);
    assert(receipt.poll_due(51, reuse_due) == Status::NotReady);
    assert(reuse_due.handle.domain == 0 && reuse_due.actor_id == 0);
    assert(receipt.claim_start(reused, 314, 1, 51, reuse_start) == Status::Ok);
    assert(reuse_start.claimed_at_ns == 51);
    assert(receipt.claim_start(reused, 314, 1, 52, reuse_start) == Status::Stale);
    assert(reuse_start.claimed_at_ns == 0 && reuse_start.deadline_ns == 0);
    receipt.close();
    reuse_due.actor_id = 314;
    assert(receipt.poll_due(53, reuse_due) == Status::Closed);
    assert(reuse_due.actor_id == 0);
    next_deadline = 12345;
    assert(receipt.next_deadline_ns(next_deadline) == Status::Closed);
    assert(next_deadline == 0);
    reused = saved;
    assert(receipt.arm_after(54, 0, 314, 1, 0, reused) == Status::Closed);
    assert(reused.domain == 0);
    // Reserve an explicitly nonzero claim-to-guest-entry bound, failing
    // before guest entry if its worst-case remainder exceeds the deadline.
    Queue bounded;
    Queue::Handle bounded_h{};
    Queue::Expired bounded_due{};
    Queue::Start bounded_start{};
    assert(bounded.arm_after(100, 5, 999, 1, 2, bounded_h) == Status::Ok);
    assert(bounded.poll_due(105, bounded_due) == Status::Ok);
    assert(bounded.claim_start_with_dispatch_bound(
               bounded_h, 999, 1, 105, 0, bounded_start) == Status::Invalid);
    assert(bounded.outstanding() == 1);
    assert(bounded_start.deadline_ns == 0);
    assert(bounded.claim_start_with_dispatch_bound(
               bounded_h, 999, 1, 106, 1, bounded_start) == Status::Ok);
    assert(bounded_start.dispatch_bound_applied);
    assert(bounded_start.dispatch_bound_ns == 1);
    assert(bounded_start.lateness_ns == 1);
    assert(bounded.outstanding() == 0);

    Queue insufficient;
    Queue::Handle insufficient_h{};
    assert(insufficient.arm_after(200, 0, 999, 1, 2, insufficient_h)
           == Status::Ok);
    assert(insufficient.poll_due(200, bounded_due) == Status::Ok);
    assert(insufficient.claim_start_with_dispatch_bound(
               insufficient_h, 999, 1, 201, 2, bounded_start)
           == Status::DeadlineMissed);
    assert(insufficient.outstanding() == 0);
    assert(bounded_start.dispatch_bound_ns == 2);

    // UINT64_MAX lateness plus a positive reserve must miss, not wrap.
    Queue overflow_reserve;
    Queue::Handle overflow_h{};
    assert(overflow_reserve.arm_after(0, 0, 999, 1, UINT64_MAX, overflow_h)
           == Status::Ok);
    assert(overflow_reserve.poll_due(0, bounded_due) == Status::Ok);
    assert(overflow_reserve.claim_start_with_dispatch_bound(
               overflow_h, 999, 1, UINT64_MAX, 1, bounded_start)
           == Status::DeadlineMissed);
    assert(bounded_start.lateness_ns == UINT64_MAX);
    sweep_dispatch_budget_invariants();
    return 0;
}
