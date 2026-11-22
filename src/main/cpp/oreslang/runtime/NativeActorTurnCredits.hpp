#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace oreslang::runtime {

/**
 * Host-side fixed-capacity, nonparking physical actor turn credits.
 *
 * This is NOT attached to NativeActorScheduler or source actor dispatch yet.
 * The current scheduler/execution authority still grants one exclusive actor
 * claim. Do not use this gate alone to bypass that authority or to run the
 * same actor's mutable heap on multiple native carriers.
 *
 * No heap allocation, trace GC, locks, waiting or guest callbacks in try_acquire,
 * release, retire, or in_flight. CAS failure returns immediately.
 */
class NativeActorTurnCredits final {
public:
    static constexpr std::size_t default_capacity = 1;
    static constexpr std::size_t maximum_capacity = 3;

    struct Claim {
        std::uint8_t slot;
        std::uint64_t ticket;
    };

    explicit NativeActorTurnCredits(std::size_t capacity = default_capacity)
        : capacity_(capacity) {
        if (capacity < 1 || capacity > maximum_capacity)
            throw std::invalid_argument("physical actor turn capacity must be in 1..3");
        for (auto& ticket : tickets_)
            ticket.store(0, std::memory_order_relaxed);
    }

    NativeActorTurnCredits(const NativeActorTurnCredits&) = delete;
    NativeActorTurnCredits& operator=(const NativeActorTurnCredits&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    std::optional<Claim> try_acquire() noexcept {
        if (retired_.load(std::memory_order_acquire)) return std::nullopt;
        for (std::size_t i = 0; i < capacity_; ++i) {
            auto current = tickets_[i].load(std::memory_order_acquire);
            if ((current & 1u) != 0 || current >= std::numeric_limits<std::uint64_t>::max() - 1)
                continue;
            if (!tickets_[i].compare_exchange_strong(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                continue; // collision: no spinning/parking on the carrier
            Claim claim{static_cast<std::uint8_t>(i), current + 1};
            if (retired_.load(std::memory_order_acquire)) {
                (void)release(claim); // close wins late admission
                return std::nullopt;
            }
            return claim;
        }
        return std::nullopt;
    }

    // A copied/stale ticket cannot retire a later claim; terminal release
    // is single-winner. The host must also authenticate actor/thread identity.
    bool release(Claim claim) noexcept {
        if (claim.slot >= capacity_ || (claim.ticket & 1u) == 0
                || claim.ticket == std::numeric_limits<std::uint64_t>::max())
            return false;
        auto expected = claim.ticket;
        return tickets_[claim.slot].compare_exchange_strong(
            expected, claim.ticket + 1, std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }

    void retire() noexcept { retired_.store(true, std::memory_order_release); }
    bool is_retired() const noexcept { return retired_.load(std::memory_order_acquire); }

    std::size_t in_flight() const noexcept {
        std::size_t total = 0;
        for (std::size_t i = 0; i < capacity_; ++i)
            total += (tickets_[i].load(std::memory_order_acquire) & 1u) != 0;
        return total;
    }

private:
    const std::size_t capacity_;
    std::array<std::atomic<std::uint64_t>, maximum_capacity> tickets_{};
    std::atomic<bool> retired_{false};
};
} // namespace oreslang::runtime
