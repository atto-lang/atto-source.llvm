#pragma once

#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

/**
 * Bounded, single-subscriber i64 demand adapter over the existing native
 * channel. It does not create an executor, a waiter queue or a guest Flow ABI.
 *
 * A successful offer consumes one credit; a rejected offer does not. Queued
 * and checked-out (not yet ACKed) scalar bytes remain charged until retired.
 * This is a trusted host/runtime building block, NOT a proof of guest move
 * checking, cross-thread actor isolation, or a zero-allocation compiler path.
 *
 * Capacity=0 rendezvous is deliberately outside this try-only adapter; use
 * the underlying native select/continuation protocol for genuine rendezvous.
 */
template <std::size_t Capacity,
          std::size_t MaxInFlight = 4,
          std::size_t MaxBytes = (Capacity + MaxInFlight) * sizeof(std::int64_t),
          std::size_t MaxDemand = 1024>
class NativeDemandI64Port final {
    static_assert(Capacity > 0 && MaxInFlight > 0, "positive bounded capacities required");
    static_assert(MaxBytes >= sizeof(std::int64_t), "byte budget too small");
    static_assert(MaxDemand > 0, "demand must be bounded and positive");
    static_assert(Capacity <= 65536 && MaxInFlight <= 4096,
                  "descriptor budgets exceed implementation ceiling");

public:
    enum class Offer { Accepted, NoSubscriber, NoDemand, Full, ByteBudget, Closed };
    enum class PollStatus { Item, NotReady, Complete, Cancelled };

    struct Poll {
        PollStatus status = PollStatus::NotReady;
        std::int64_t value = 0;
        std::uint64_t ticket = 0;
    };

    NativeDemandI64Port() = default;
    NativeDemandI64Port(const NativeDemandI64Port&) = delete;
    NativeDemandI64Port& operator=(const NativeDemandI64Port&) = delete;

    /** Claims the only logical consumer; there is no second lease or multicast. */
    bool subscribe() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (subscribed_ || finished_ || cancelled_) return false;
        subscribed_ = true;
        return true;
    }

    /** Positive bounded request; throws before mutating on overflow/exhaustion. */
    void request(std::size_t n) {
        if (!n) throw std::invalid_argument("request must be positive");
        std::lock_guard<std::mutex> guard(mutex_);
        if (!subscribed_ || finished_ || cancelled_)
            throw std::logic_error("cannot request without active subscriber");
        if (n > MaxDemand - credits_)
            throw std::invalid_argument("outstanding demand budget exceeded");
        credits_ += n;
    }

    Offer try_offer(std::int64_t value) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (finished_ || cancelled_) return Offer::Closed;
        if (!subscribed_) return Offer::NoSubscriber;
        if (!credits_) return Offer::NoDemand;
        if (used_bytes_ > MaxBytes - sizeof(value)) return Offer::ByteBudget;
        if (!channel_.try_write(value)) return Offer::Full;
        --credits_;
        used_bytes_ += sizeof(value);
        return Offer::Accepted;
    }

    Poll try_next() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (cancelled_) return {PollStatus::Cancelled, 0, 0};
        if (!subscribed_) return {};
        std::size_t free_slot = MaxInFlight;
        for (std::size_t i = 0; i < MaxInFlight; ++i) {
            if (!loans_[i].active) { free_slot = i; break; }
        }
        if (free_slot == MaxInFlight) return {};
        if (next_ticket_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("delivery ticket space exhausted");
        const auto outcome = channel_.try_read_result();
        if (outcome.status == Channel::ReadStatus::Eof)
            return {PollStatus::Complete, 0, 0};
        if (outcome.status == Channel::ReadStatus::WouldBlock)
            return {};
        const auto ticket = next_ticket_++;
        loans_[free_slot] = {true, ticket};
        return {PollStatus::Item, outcome.value, ticket};
    }

    /** Cancelling does not free values still physically held by a consumer. */
    bool ack(std::uint64_t ticket) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& loan : loans_) {
            if (loan.active && loan.ticket == ticket) {
                loan.active = false;
                used_bytes_ -= sizeof(std::int64_t);
                return true;
            }
        }
        return false;
    }

    /** Close drains committed values first, then emits COMPLETE. */
    bool complete() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (finished_ || cancelled_) return false;
        finished_ = true;
        credits_ = 0;
        (void)channel_.close();
        return true;
    }

    /** Cancel drops queued items exactly once; in-flight loans survive until ACK. */
    bool cancel() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (finished_ || cancelled_) return false;
        cancelled_ = true;
        finished_ = true;
        credits_ = 0;
        (void)channel_.close();
        while (channel_.try_read().has_value()) used_bytes_ -= sizeof(std::int64_t);
        return true;
    }

    std::size_t retained_bytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return used_bytes_;
    }
    std::size_t queued() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return channel_.size();
    }
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        std::size_t count = 0;
        for (const auto& loan : loans_) if (loan.active) ++count;
        return count;
    }
    std::size_t outstanding_demand() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return credits_;
    }

private:
    using Channel = NativeSelectableI64Channel<Capacity>;
    struct Loan { bool active = false; std::uint64_t ticket = 0; };
    mutable std::mutex mutex_;
    Channel channel_;
    std::array<Loan, MaxInFlight> loans_{};
    bool subscribed_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
    std::size_t credits_ = 0;
    std::size_t used_bytes_ = 0;
    std::uint64_t next_ticket_ = 1;
};

} // namespace oreslang::runtime
