#pragma once
#include "oreslang/runtime/AsyncIoCompletionGate.hpp"
#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

// Source-compiler prerequisite only: caller owns the actor-pinned allocation.
// A broker may set terminal/ACK atomics, but never access actor-owned fields.
// The owner MUST retain the frame and broker callbacks until retirement.
// This gate cannot itself prove that the compiler pinned a source-level owner.
class PinnedContinuationGate final {
public:
    enum class Loan : std::uint8_t { shared, exclusive };
    // Host/FFI values must be validated before treating an unknown mode as
    // shared. A forged release mode must not drop another owner's loan.
    static constexpr bool valid_loan_mode(Loan mode) noexcept {
        return mode == Loan::shared || mode == Loan::exclusive;
    }
    static constexpr std::uint32_t exclusive_bit = 0x80000000u;
    static constexpr std::uint32_t retirement_fence_bit = 0x40000000u;
    static constexpr std::uint32_t control_bits = exclusive_bit | retirement_fence_bit;
    static constexpr std::uint32_t max_shared_loans = 1024;
    // A future layout change must never overlap live shared counts with
    // exclusive admission or the permanent retirement fence.
    static_assert((exclusive_bit & retirement_fence_bit) == 0,
                  "loan control bits must be disjoint");
    static_assert(max_shared_loans > 0 && max_shared_loans < retirement_fence_bit,
                  "shared loan bound must not reach a control bit");

    explicit PinnedContinuationGate(AsyncIoTicket ticket) noexcept : ticket_(ticket) {}
    PinnedContinuationGate(const PinnedContinuationGate&) = delete;
    PinnedContinuationGate& operator=(const PinnedContinuationGate&) = delete;

    bool acquire(AsyncIoTicket ticket, Loan mode) noexcept {
        if (!valid(ticket) || !valid_loan_mode(mode)
                || terminal_.load(std::memory_order_acquire) != AsyncIoTerminal::pending)
            return false;
        std::uint32_t old = loans_.load(std::memory_order_acquire);
        for (;;) {
            if (mode == Loan::exclusive ? old != 0 : (old & control_bits) || old >= max_shared_loans)
                return false;
            const std::uint32_t next = mode == Loan::exclusive ? exclusive_bit : old + 1;
            if (loans_.compare_exchange_weak(old, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // A completion racing a new loan may invalidate the acquisition.
                if (terminal_.load(std::memory_order_acquire) != AsyncIoTerminal::pending) {
                    release(ticket, mode);
                    return false;
                }
                return true;
            }
        }
    }

    bool release(AsyncIoTicket ticket, Loan mode) noexcept {
        if (!valid(ticket) || !valid_loan_mode(mode)) return false;
        std::uint32_t old = loans_.load(std::memory_order_acquire);
        for (;;) {
            if (mode == Loan::exclusive ? old != exclusive_bit
                    : old == 0 || (old & control_bits)) return false;
            const std::uint32_t next = mode == Loan::exclusive ? 0 : old - 1;
            if (loans_.compare_exchange_weak(old, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }

    // One winner for completion, timeout, or cancellation. Cancellation does
    // NOT imply that the I/O broker has quiesced or that memory can be freed.
    bool settle(AsyncIoTicket ticket, AsyncIoTerminal outcome) noexcept {
        if (!valid(ticket) || !valid_async_io_terminal(outcome)) return false;
        AsyncIoTerminal old = AsyncIoTerminal::pending;
        return terminal_.compare_exchange_strong(old, outcome,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Only call after the broker has stopped touching its I/O-owned buffers,
    // withdrawn registrations, and can no longer issue a completion callback.
    bool broker_acknowledge(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket) || terminal_.load(std::memory_order_acquire) == AsyncIoTerminal::pending)
            return false;
        bool old = false;
        return broker_quiesced_.compare_exchange_strong(old, true,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Actor scheduler only. Claiming a resume/cleanup turn NEVER finishes it.
    bool try_claim_resume(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket) || terminal_.load(std::memory_order_acquire) == AsyncIoTerminal::pending
                || !broker_quiesced_.load(std::memory_order_acquire)) return false;
        bool old = false;
        return resume_claimed_.compare_exchange_strong(old, true,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool finish_resume(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket) || !resume_claimed_.load(std::memory_order_acquire)) return false;
        bool old = false;
        return resume_finished_.compare_exchange_strong(old, true,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Must be called by the lifetime owner AFTER all live loans were released,
    // the I/O broker ACK'd, and one guest cleanup/resume TURN FINISHED.
    bool try_retire(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket) || !broker_quiesced_.load(std::memory_order_acquire)
                || !resume_finished_.load(std::memory_order_acquire)) return false;
        // Fence retirement in the SAME atomic word as loan admission.
        // A separate loans_==0 check and retired_ CAS permits a stale
        // acquirer to enter after physical retirement was authorized.
        std::uint32_t expected = 0;
        if (!loans_.compare_exchange_strong(expected, retirement_fence_bit,
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        // This one-winner loan-word fence also claims retirement.
        retired_.store(true, std::memory_order_release);
        return true;
    }

    AsyncIoTerminal terminal() const noexcept { return terminal_.load(std::memory_order_acquire); }
    bool retired() const noexcept { return retired_.load(std::memory_order_acquire); }
    std::uint32_t loan_state() const noexcept { return loans_.load(std::memory_order_acquire); }

private:
    bool valid(AsyncIoTicket ticket) const noexcept {
        return ticket.actor_id && ticket.generation && ticket.operation_id
            && ticket.actor_id == ticket_.actor_id
            && ticket.generation == ticket_.generation
            && ticket.operation_id == ticket_.operation_id;
    }
    const AsyncIoTicket ticket_;
    std::atomic<AsyncIoTerminal> terminal_{AsyncIoTerminal::pending};
    std::atomic<std::uint32_t> loans_{0};
    std::atomic<bool> broker_quiesced_{false};
    std::atomic<bool> resume_claimed_{false};
    std::atomic<bool> resume_finished_{false};
    std::atomic<bool> retired_{false};
};
} // namespace oreslang::runtime
