#pragma once

#include "oreslang/runtime/ActorHeapIdentity.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

class HungryCarrier;

/**
 * Minted only after HungryCarrier::join() has stopped actor and broker,
 * closed ingress, and drained every outstanding producer call.
 *
 * A proof does NOT cover unrelated FFI borrows: those must be tracked with
 * ActorHeapBorrowGate below and drained separately.
 */
class CarrierRetirementProof final {
    friend class HungryCarrier;
    ActorHeapIdentity owner_;
    explicit CarrierRetirementProof(ActorHeapIdentity owner) : owner_(owner) {}
public:
    ActorHeapIdentity owner() const noexcept { return owner_; }
};

/**
 * Bounded admission guard for actor-heap borrows, async frames and FFI leases.
 * Callers MUST obtain a lease before publishing any native heap reference
 * outside its owning actor turn. Closing the gate bars new borrows; retirement
 * is allowed only after every prior borrow was released.
 *
 * This is an admission protocol, not a memory-protection mechanism against
 * malicious native code. Gate storage must outlive all acquired leases.
 */
class ActorHeapBorrowGate final {
    static constexpr std::uint64_t closing_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t counter_mask = closing_bit - 1;
    ActorHeapIdentity owner_;
    std::atomic<std::uint64_t> state_{0};

public:
    explicit ActorHeapBorrowGate(ActorHeapIdentity owner) : owner_(owner) {}
    ActorHeapBorrowGate(const ActorHeapBorrowGate&) = delete;
    ActorHeapBorrowGate& operator=(const ActorHeapBorrowGate&) = delete;

    class Lease final {
        friend class ActorHeapBorrowGate;
        ActorHeapBorrowGate* gate_ = nullptr;
        explicit Lease(ActorHeapBorrowGate* gate) noexcept : gate_(gate) {}
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Lease() { reset(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        void reset() noexcept {
            if (auto* gate = std::exchange(gate_, nullptr))
                gate->state_.fetch_sub(1, std::memory_order_release);
        }
    };

    std::optional<Lease> acquire(ActorHeapIdentity actor) {
        if (actor != owner_) throw std::logic_error("foreign heap borrow admission");
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current & closing_bit) return std::nullopt;
            if ((current & counter_mask) == counter_mask)
                throw std::overflow_error("actor heap borrower count overflow");
            if (state_.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return Lease{this};
        }
    }

    // Seal is monotonic and cannot lose a racing borrower. The caller retries
    // quiescence only after outstanding leases/FFI callbacks have drained.
    bool seal_and_quiesced(ActorHeapIdentity actor) {
        if (actor != owner_) throw std::logic_error("foreign actor heap retirement");
        const auto previous = state_.fetch_or(closing_bit, std::memory_order_acq_rel);
        return (previous & counter_mask) == 0;
    }
    bool quiesced() const noexcept {
        return state_.load(std::memory_order_acquire) == closing_bit;
    }
    bool sealed() const noexcept {
        return (state_.load(std::memory_order_acquire) & closing_bit) != 0;
    }
};

} // namespace oreslang::runtime
