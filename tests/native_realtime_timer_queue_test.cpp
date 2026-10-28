#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"
#include <cassert>
#include <cstdint>
#include <limits>

using Queue = oreslang::runtime::realtime::HardRealtimeTimerQueue<2>;
using Status = Queue::Status;

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
    assert(q.cancel(z) == Status::Stale);  // Cross-scheduler domain.
    q.close();
    assert(q.outstanding() == 0);
    assert(q.poll_due(UINT64_MAX, expiry) == Status::Closed);
    assert(q.arm_after(UINT64_MAX, 0, 7, 2, 1, z) == Status::Closed);
    assert(q2.cancel(z) == Status::Ok);

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
    return 0;
}
