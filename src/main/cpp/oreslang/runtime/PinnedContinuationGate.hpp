#pragma once
#include "oreslang/runtime/AsyncIoCompletionGate.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace oreslang::runtime {

// Source-compiler prerequisite only: caller owns the actor-pinned allocation.
// A broker may set terminal/ACK atomics, but never access actor-owned fields.
// The owner MUST retain the frame and broker callbacks until retirement.
// This gate cannot itself prove that the compiler pinned a source-level owner.
class PinnedContinuationGate final {
public:
    enum class Loan : std::uint8_t { shared, exclusive };
    static constexpr std::uint32_t exclusive_bit = 0x80000000u;
    static constexpr std::uint32_t max_shared_loans = 1024;

    explicit PinnedContinuationGate(AsyncIoTicket ticket) noexcept : ticket_(ticket) {}

    /**
     * Bind this continuation frame to one actor-owned heap while still inside
     * the admitted actor turn. The external lease is lifetime-only: it carries
     * no slot index and cannot read/write actor state. Physical heap destroy is
     * blocked until successful try_retire() releases this token.
     */
    PinnedContinuationGate(
            AsyncIoTicket ticket,
            OresActorI64Heap* heap,
            OresActorTurn turn)
        : ticket_(ticket) {
        if (heap == nullptr || ticket.actor_id == 0
                || ticket.generation == 0 || ticket.operation_id == 0) {
            std::abort();
        }
        lifetime_heap_ = heap;
        lifetime_lease_ = ores_actor_i64_heap_external_acquire(heap, turn);
    }

    PinnedContinuationGate(const PinnedContinuationGate&) = delete;
    PinnedContinuationGate& operator=(const PinnedContinuationGate&) = delete;

    /**
     * Lifetime authority must be retired explicitly after broker quiescence,
     * guest cleanup and loan release. Silently destroying a still-pinned gate
     * would leak the external heap lease forever; releasing it here would be
     * worse because callbacks/borrows may still exist. Fail closed instead.
     */
    ~PinnedContinuationGate() noexcept {
        if (lifetime_heap_ != nullptr) std::abort();
    }

    bool acquire(AsyncIoTicket ticket, Loan mode) noexcept {
        if (!valid(ticket) || terminal_.load(std::memory_order_acquire) != AsyncIoTerminal::pending)
            return false;
        std::uint32_t old = loans_.load(std::memory_order_acquire);
        for (;;) {
            if (mode == Loan::exclusive ? old != 0 : (old & exclusive_bit) || old >= max_shared_loans)
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
        if (!valid(ticket)) return false;
        std::uint32_t old = loans_.load(std::memory_order_acquire);
        for (;;) {
            if (mode == Loan::exclusive ? old != exclusive_bit
                    : old == 0 || (old & exclusive_bit)) return false;
            const std::uint32_t next = mode == Loan::exclusive ? 0 : old - 1;
            if (loans_.compare_exchange_weak(old, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }

    // One winner for completion, timeout, or cancellation. Cancellation does
    // NOT imply that the I/O broker has quiesced or that memory can be freed.
    bool settle(AsyncIoTicket ticket, AsyncIoTerminal outcome) noexcept {
        if (!valid(ticket) || outcome == AsyncIoTerminal::pending) return false;
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
                || !resume_finished_.load(std::memory_order_acquire)
                || loans_.load(std::memory_order_acquire) != 0) return false;
        bool old = false;
        if (!retired_.compare_exchange_strong(old, true,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        if (lifetime_heap_ != nullptr) {
            ores_actor_i64_heap_external_release(
                    lifetime_heap_, &lifetime_lease_);
            lifetime_heap_ = nullptr;
        }
        return true;
    }

    bool lifetime_pinned() const noexcept {
        return lifetime_heap_ != nullptr
            && lifetime_lease_.domain != 0
            && lifetime_lease_.generation != 0;
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
    OresActorI64Heap* lifetime_heap_{nullptr};
    OresActorExternalLease lifetime_lease_{0, 0, 0};
    std::atomic<AsyncIoTerminal> terminal_{AsyncIoTerminal::pending};
    std::atomic<std::uint32_t> loans_{0};
    std::atomic<bool> broker_quiesced_{false};
    std::atomic<bool> resume_claimed_{false};
    std::atomic<bool> resume_finished_{false};
    std::atomic<bool> retired_{false};
};
} // namespace oreslang::runtime
