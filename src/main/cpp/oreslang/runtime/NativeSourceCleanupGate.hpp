#pragma once

#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

// Host-only, pointer-free cancellation/unwind receipt. Caller MUST keep this
// object alive until all scheduler/CONTROL users are quiescent. It neither
// dispatches guest cleanup nor proves correct source lowering by itself.
class NativeSourceCleanupBinding;

struct NativeSourceCleanupTicket {
    std::uint64_t actor_id;
    std::uint64_t actor_generation;
    std::uint64_t continuation_id;
};

class NativeSourceCleanupGate final {
public:
    enum class Phase : std::uint8_t {
        pending, cancel_requested, cleanup_running, cleanup_yielded,
        completed, cleanup_incomplete
    };

    explicit NativeSourceCleanupGate(NativeSourceCleanupTicket ticket) noexcept
        : ticket_(ticket) {}
    NativeSourceCleanupGate(const NativeSourceCleanupGate&) = delete;
    NativeSourceCleanupGate& operator=(const NativeSourceCleanupGate&) = delete;

    // Completion vs cancellation is a one-winner CAS. Positive physical
    // release must be asserted by a store-checked scheduler binding only.
    // A naked actor/generation/continuation tuple is not completion authority.
private:
    friend class NativeSourceCleanupBinding;
#ifdef ORESLANG_NATIVE_SOURCE_CLEANUP_TEST_PEER
    friend struct NativeSourceCleanupGateTestPeer;
#endif
    bool complete_normally(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::pending;
        return phase_.compare_exchange_strong(expected, Phase::completed,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

public:
    // A cancel REQUEST never publishes a physical-completion receipt.
    bool request_cancel(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::pending;
        return phase_.compare_exchange_strong(expected, Phase::cancel_requested,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    // Only the store-checked runtime binding may mint positive cleanup
    // progress in production builds. The direct test peer is compiled
    // exclusively into focused native CTests.
    friend class NativeSourceCleanupBinding;
#ifdef ORESLANG_NATIVE_SOURCE_CLEANUP_TEST_PEER
    friend struct NativeSourceCleanupGateTestPeer;
#endif

    // Trusted actor scheduler only; never called directly by a broker thread.
    // A cleanup continuation may yield/await and be claimed again later.
    bool try_claim_cleanup(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::cancel_requested;
        if (phase_.compare_exchange_strong(expected, Phase::cleanup_running,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        expected = Phase::cleanup_yielded;
        return phase_.compare_exchange_strong(expected, Phase::cleanup_running,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool yield_cleanup(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::cleanup_running;
        return phase_.compare_exchange_strong(expected, Phase::cleanup_yielded,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Called only by the trusted scheduler AFTER LIFO defers, finally,
    // iterator close, and all guard/borrow/FFI retirement obligations have
    // physically completed. This function cannot independently verify them.
    bool complete_cleanup(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::cleanup_running;
        return phase_.compare_exchange_strong(expected, Phase::completed,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

public:
    // Supervisor failure/quarantine is always fail-closed, never a positive
    // physical release. Only the binding may mint a successful cleanup turn.
    // Scheduler close, timeout, cleanup panic or missing owner execution lane
    // must never be mistaken for a positive physical cleanup receipt.
    bool fail_cleanup(NativeSourceCleanupTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase old = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (old != Phase::cancel_requested && old != Phase::cleanup_running
                    && old != Phase::cleanup_yielded) return false;
            if (phase_.compare_exchange_weak(old, Phase::cleanup_incomplete,
                        std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }

    // Bind a receipt to exactly the actor incarnation being retired.
    // Continuation-specific completion does NOT replace the registry's duty
    // to pin every other suspended continuation and live child/FFI loan.
    bool belongs_to(std::uint64_t actor_id, std::uint64_t generation) const noexcept {
        return actor_id != 0 && generation != 0 && ticket_.continuation_id != 0
            && ticket_.actor_id == actor_id && ticket_.actor_generation == generation;
    }

    Phase phase() const noexcept { return phase_.load(std::memory_order_acquire); }
    bool ready_for_reclaim() const noexcept { return phase() == Phase::completed; }
    bool incomplete() const noexcept { return phase() == Phase::cleanup_incomplete; }

private:
    bool valid(NativeSourceCleanupTicket ticket) const noexcept {
        return ticket.actor_id != 0 && ticket.actor_generation != 0
            && ticket.continuation_id != 0 && ticket.actor_id == ticket_.actor_id
            && ticket.actor_generation == ticket_.actor_generation
            && ticket.continuation_id == ticket_.continuation_id;
    }

    const NativeSourceCleanupTicket ticket_;
    std::atomic<Phase> phase_{Phase::pending};
};

} // namespace oreslang::runtime
