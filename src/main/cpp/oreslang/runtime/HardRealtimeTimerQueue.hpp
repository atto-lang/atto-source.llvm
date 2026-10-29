#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime::realtime {

namespace detail {
// One process-global identity source for *all* template pool capacities.
// A template-local static counter would permit duplicate domain identities
// between HardRealtimeTimerQueue<2> and HardRealtimeTimerQueue<4>.
inline std::uint64_t next_realtime_timer_domain() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate != 0 && candidate != UINT64_MAX) {
        if (next.compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed)) return candidate;
    }
    return 0;
}
}  // namespace detail

// Bootstrap-allocated, single-owner CONTROL scheduler data structure.
// NOT thread-safe: cross-thread requests must enter through a separately
// bounded, preallocated ingress queue. No guest pointers or callbacks.
template<std::size_t Capacity>
class HardRealtimeTimerQueue final {
    static_assert(Capacity > 0 && Capacity <= UINT32_MAX, "invalid timer capacity");
public:
    enum class Status : std::uint8_t {
        Ok, Full, Invalid, ClockBackwards, Overflow, NotReady, Stale,
        DeadlineMissed, Closed
    };
    struct Handle {
        std::uint64_t domain = 0;
        std::uint64_t generation = 0;
        std::uint32_t slot = 0;
    };
    struct Expired {
        Handle handle{};
        std::uint64_t deadline_ns = 0;
        // Clock sample when expiry was recognized, NOT CONTROL-ready or guest start.
        std::uint64_t observed_expiry_ns = 0;
        std::uint64_t actor_id = 0;
        std::uint64_t actor_generation = 0;
    };
    struct Start {
        // Timestamp is taken by the scheduler at claim, before guest entry;
        // subsequent scheduling/preemption time requires separate measurement.
        std::uint64_t deadline_ns = 0;
        std::uint64_t claimed_at_ns = 0;
        std::uint64_t maximum_lateness_ns = 0;
        std::uint64_t lateness_ns = 0;
        // Conservative scheduler-claim-to-guest-entry reserve. Zero indicates
        // an unqualified legacy claim, NOT proof of hard-RT guest entry.
        std::uint64_t dispatch_bound_ns = 0;
        bool dispatch_bound_applied = false;
    };

    HardRealtimeTimerQueue() noexcept : domain_(detail::next_realtime_timer_domain()) {}
    HardRealtimeTimerQueue(const HardRealtimeTimerQueue&) = delete;
    HardRealtimeTimerQueue& operator=(const HardRealtimeTimerQueue&) = delete;

    // Immutable timer-to-start budget is fixed at timer admission.
    // It cannot be loosened by a late dispatcher.
    Status arm_after(std::uint64_t now_ns, std::uint64_t delay_ns,
                     std::uint64_t actor_id, std::uint64_t actor_generation,
                     std::uint64_t maximum_lateness_ns, Handle& out) noexcept {
        // Failed admission must not leave a reusable or misleading ticket.
        out = Handle{};
        if (closed_) return Status::Closed;
        if (domain_ == 0 || actor_id == 0 || actor_generation == 0)
            return Status::Invalid;
        // Rejected admissions must not advance the clock watermark: an
        // overflowing or full request with a far-future timestamp must not
        // prevent an already-armed timer from subsequently being polled.
        // Commit the observation only when a slot is actually admitted.
        if (clock_seen_ && now_ns < last_now_ns_) return Status::ClockBackwards;
        if (delay_ns > UINT64_MAX - now_ns) return Status::Overflow;
        if (next_sequence_ == UINT64_MAX) return Status::Overflow;
        for (std::size_t i = 0; i < Capacity; ++i) {
            Slot& s = slots_[i];
            if (s.phase != Phase::Free || s.generation == UINT64_MAX) continue;
            clock_seen_ = true;
            last_now_ns_ = now_ns;
            ++s.generation;
            s.sequence = next_sequence_++;
            s.deadline_ns = now_ns + delay_ns;
            s.maximum_lateness_ns = maximum_lateness_ns;
            s.actor_id = actor_id;
            s.actor_generation = actor_generation;
            s.phase = Phase::Armed;
            out = Handle{domain_, s.generation, static_cast<std::uint32_t>(i)};
            ++outstanding_;
            return Status::Ok;
        }
        return Status::Full;
    }

    // Expiry is readiness only; caller enqueues an opaque token to actor CONTROL.
    Status poll_due(std::uint64_t now_ns, Expired& out) noexcept {
        // A NotReady/error status must not expose a previous expiry receipt.
        out = Expired{};
        if (closed_) return Status::Closed;
        if (!observe_clock(now_ns)) return Status::ClockBackwards;
        std::size_t best = Capacity;
        for (std::size_t i = 0; i < Capacity; ++i) {
            const Slot& s = slots_[i];
            if (s.phase != Phase::Armed || s.deadline_ns > now_ns) continue;
            if (best == Capacity || s.deadline_ns < slots_[best].deadline_ns ||
                (s.deadline_ns == slots_[best].deadline_ns &&
                 s.sequence < slots_[best].sequence)) best = i;
        }
        if (best == Capacity) return Status::NotReady;
        Slot& s = slots_[best];
        s.phase = Phase::Ready;
        out = Expired{Handle{domain_, s.generation, static_cast<std::uint32_t>(best)},
                      s.deadline_ns, now_ns, s.actor_id, s.actor_generation};
        return Status::Ok;
    }

    // Cancellation remains legal after expiry, until the actor CONTROL start claim.
    Status cancel(Handle h) noexcept {
        if (closed_) return Status::Closed;
        Slot* s = locate(h);
        if (!s) return Status::Stale;
        retire(*s);
        return Status::Ok;
    }

    // Actor STOP/hot-reload revokes ARMED and READY timers for one incarnation.
    // O(Capacity), one-writer only, with no allocation or callback execution.
    Status revoke_actor(std::uint64_t actor_id, std::uint64_t actor_generation,
                        std::size_t& revoked) noexcept {
        revoked = 0;
        if (closed_) return Status::Closed;
        if (actor_id == 0 || actor_generation == 0) return Status::Invalid;
        for (Slot& s : slots_) {
            if (s.phase != Phase::Free && s.actor_id == actor_id &&
                s.actor_generation == actor_generation) {
                retire(s);
                ++revoked;
            }
        }
        return Status::Ok;
    }

    // Earliest ARMED deadline, for a separately qualified absolute OS wakeup.
    Status next_deadline_ns(std::uint64_t& out) const noexcept {
        out = 0;
        if (closed_) return Status::Closed;
        bool found = false;
        for (const Slot& s : slots_) {
            if (s.phase != Phase::Armed) continue;
            if (!found || s.deadline_ns < out) out = s.deadline_ns;
            found = true;
        }
        return found ? Status::Ok : Status::NotReady;
    }

    // Legacy claim is an actor-carrier claim timestamp, NOT a verified guest
    // first-instruction timestamp. It must never qualify a hard-RT timer.
    Status claim_start(Handle h, std::uint64_t actor_id,
                       std::uint64_t actor_generation, std::uint64_t now_ns,
                       Start& out) noexcept {
        return claim_start_internal(h, actor_id, actor_generation, now_ns,
                                    0, false, out);
    }

    // Conservative admission before guest entry: reserve a nonzero *qualified*
    // upper bound for scheduler-claim-to-first-guest-instruction latency.
    // The caller must prove this upper bound from its actual code path and
    // separately sample the real guest prologue. No hard-RT guarantee follows
    // from this helper or a test-supplied clock alone.
    Status claim_start_with_dispatch_bound(
            Handle h, std::uint64_t actor_id,
            std::uint64_t actor_generation, std::uint64_t now_ns,
            std::uint64_t dispatch_bound_ns, Start& out) noexcept {
        out = Start{};
        if (dispatch_bound_ns == 0) return Status::Invalid;
        return claim_start_internal(h, actor_id, actor_generation, now_ns,
                                    dispatch_bound_ns, true, out);
    }

    void close() noexcept {
        closed_ = true;
        // Permanent close scrubs actor identity and retained deadline data.
        for (Slot& s : slots_) s = Slot{};
        outstanding_ = 0;
    }
    std::size_t outstanding() const noexcept { return outstanding_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    Status claim_start_internal(
            Handle h, std::uint64_t actor_id,
            std::uint64_t actor_generation, std::uint64_t now_ns,
            std::uint64_t dispatch_bound_ns, bool bound_applied,
            Start& out) noexcept {
        // Invalid/stale start attempts cannot expose old output receipts.
        out = Start{};
        if (closed_) return Status::Closed;
        Slot* s = locate(h);
        if (!s || s->phase != Phase::Ready || s->actor_id != actor_id ||
            s->actor_generation != actor_generation) return Status::Stale;
        if (!observe_clock(now_ns)) return Status::ClockBackwards;
        if (now_ns < s->deadline_ns) return Status::ClockBackwards;
        out.deadline_ns = s->deadline_ns;
        out.claimed_at_ns = now_ns;
        out.maximum_lateness_ns = s->maximum_lateness_ns;
        out.lateness_ns = now_ns - s->deadline_ns;
        out.dispatch_bound_ns = dispatch_bound_ns;
        out.dispatch_bound_applied = bound_applied;
        // Subtract remaining budget instead of adding timestamps; this
        // prevents UINT64_MAX rollover from turning a miss into a pass.
        const bool missed =
            out.lateness_ns > s->maximum_lateness_ns ||
            (bound_applied &&
             dispatch_bound_ns > s->maximum_lateness_ns - out.lateness_ns);
        retire(*s);
        return missed ? Status::DeadlineMissed : Status::Ok;
    }

    enum class Phase : std::uint8_t { Free, Armed, Ready };
    struct Slot {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        std::uint64_t deadline_ns = 0;
        std::uint64_t maximum_lateness_ns = 0;
        std::uint64_t actor_id = 0;
        std::uint64_t actor_generation = 0;
        Phase phase = Phase::Free;
    };
    bool observe_clock(std::uint64_t now) noexcept {
        if (clock_seen_ && now < last_now_ns_) return false;
        clock_seen_ = true;
        last_now_ns_ = now;
        return true;
    }
    Slot* locate(Handle h) noexcept {
        if (h.domain != domain_ || h.generation == 0 || h.slot >= Capacity)
            return nullptr;
        Slot& s = slots_[h.slot];
        return s.phase != Phase::Free && s.generation == h.generation ? &s : nullptr;
    }
    void retire(Slot& s) noexcept {
        s.phase = Phase::Free;
        s.actor_id = 0;
        s.actor_generation = 0;
        --outstanding_;
    }
    std::array<Slot, Capacity> slots_{};
    const std::uint64_t domain_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t last_now_ns_ = 0;
    std::size_t outstanding_ = 0;
    bool clock_seen_ = false;
    bool closed_ = false;
};

} // namespace oreslang::runtime::realtime
