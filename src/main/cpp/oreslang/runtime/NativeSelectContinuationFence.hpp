#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Actor-generation fence for queued select/await continuations.
 *
 * Producers register Ticket values. The actor lane must claim a ticket
 * immediately before executing the continuation. A successful claim returns an
 * Execution lease that stays live for the entire continuation invocation.
 *
 * Safety contract:
 * - ticket identity + actor generation prevent stale/ABA delivery;
 * - claim and ticket cancellation share one mutex linearization point;
 * - a claimed continuation consumes its logical ticket but retains one physical
 *   execution loan until Execution::close()/destruction;
 * - advance() rejects while any physical execution loan is live, so generation
 *   rollover cannot occur between claim and callback completion;
 * - close() cancels pending tickets but does not forge physical retirement;
 *   retirement_ready() becomes true only after all claimed executions retire;
 * - fixed MaxTickets bounds pending + executing continuations together.
 */
template <std::size_t MaxTickets = 64>
class NativeSelectContinuationFence final {
    static_assert(MaxTickets > 0, "continuation capacity must be positive");

private:
    struct Slot final {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        bool active = false;
    };

    struct State final {
        mutable std::mutex mutex;
        std::array<Slot, MaxTickets> slots{};
        std::array<std::size_t, MaxTickets> free_indices{};
        std::size_t free_count = MaxTickets;
        State() noexcept {
            for (std::size_t i = 0; i < MaxTickets; ++i)
                free_indices[i] = MaxTickets - 1 - i;
        }
        std::uint64_t generation = 1;
        std::uint64_t next_id = 1;
        std::size_t executing = 0;
        bool closed = false;
    };

    static void clear_slot(State& state, std::size_t index) noexcept {
        auto& slot = state.slots[index];
        slot.active = false;
        slot.generation = 0;
        state.free_indices[state.free_count++] = index;
    }

    static void clear_pending(State& state) noexcept {
        for (std::size_t i = 0; i < MaxTickets; ++i)
            if (state.slots[i].active) clear_slot(state, i);
    }

public:
    class Ticket final {
    public:
        Ticket() = default;

        std::uint64_t generation() const noexcept { return generation_; }

        bool active() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (index_ >= MaxTickets) return false;
            const auto& slot = state->slots[index_];
            return slot.active &&
                   slot.id == id_ &&
                   slot.generation == generation_;
        }

        bool close() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (index_ >= MaxTickets) return false;
            auto& slot = state->slots[index_];
            if (!slot.active ||
                slot.id != id_ ||
                slot.generation != generation_)
                return false;
            NativeSelectContinuationFence::clear_slot(*state, index_);
            return true;
        }

    private:
        friend class NativeSelectContinuationFence;

        Ticket(const std::shared_ptr<State>& state,
               std::size_t index,
               std::uint64_t id,
               std::uint64_t generation)
            : state_(state),
              index_(index),
              id_(id),
              generation_(generation) {}

        std::weak_ptr<State> state_;
        std::size_t index_ = MaxTickets;
        std::uint64_t id_ = 0;
        std::uint64_t generation_ = 0;
    };

    class Execution final {
    public:
        Execution() = default;
        Execution(const Execution&) = delete;
        Execution& operator=(const Execution&) = delete;

        Execution(Execution&& other) noexcept
            : state_(std::move(other.state_)),
              generation_(other.generation_),
              active_(other.active_) {
            other.generation_ = 0;
            other.active_ = false;
        }

        Execution& operator=(Execution&& other) noexcept {
            if (this == &other) return *this;
            (void)close();
            state_ = std::move(other.state_);
            generation_ = other.generation_;
            active_ = other.active_;
            other.generation_ = 0;
            other.active_ = false;
            return *this;
        }

        ~Execution() noexcept { (void)close(); }

        bool valid() const noexcept {
            return active_ && static_cast<bool>(state_);
        }

        std::uint64_t generation() const noexcept {
            return generation_;
        }

        bool close() noexcept {
            if (!active_) return false;
            auto state = std::move(state_);
            active_ = false;
            generation_ = 0;
            if (!state) return false;

            try {
                std::lock_guard<std::mutex> guard(state->mutex);
                if (state->executing == 0) return false;
                --state->executing;
                return true;
            } catch (...) {
                return false;
            }
        }

    private:
        friend class NativeSelectContinuationFence;

        Execution(std::shared_ptr<State> state,
                  std::uint64_t generation)
            : state_(std::move(state)),
              generation_(generation),
              active_(true) {}

        std::shared_ptr<State> state_;
        std::uint64_t generation_ = 0;
        bool active_ = false;
    };

    explicit NativeSelectContinuationFence(std::uint64_t generation = 1)
        : state_(std::make_shared<State>()) {
        if (generation == 0)
            throw std::invalid_argument(
                "continuation generation must be positive");
        state_->generation = generation;
    }

    // A claimed execution may retain State after the owning fence dies.
    // Revoke all queued tickets before relinquishing the owner's reference.
    ~NativeSelectContinuationFence() noexcept { (void)close(); }

    NativeSelectContinuationFence(
        const NativeSelectContinuationFence&) = delete;
    NativeSelectContinuationFence& operator=(
        const NativeSelectContinuationFence&) = delete;

    bool closed() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->closed;
    }

    std::uint64_t generation() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->generation;
    }

    std::size_t pending_tickets() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return MaxTickets - state_->free_count;
    }

    std::size_t executing() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->executing;
    }

    bool retirement_ready() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->closed && state_->executing == 0 &&
               state_->free_count == MaxTickets;
    }

    // Terminal logical close. Physical execution loans retire independently.
    bool close() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed) return false;
        state_->closed = true;
        clear_pending(*state_);
        return true;
    }

    Ticket register_continuation(std::uint64_t expected_generation) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed)
            throw std::logic_error("continuation fence is closed");
        if (expected_generation != state_->generation)
            throw std::logic_error("stale actor generation");

        if (MaxTickets - state_->free_count + state_->executing >= MaxTickets)
            throw std::overflow_error(
                "continuation physical capacity exhausted");
        if (state_->free_count == 0)
            throw std::overflow_error(
                "continuation ticket capacity exhausted");
        const auto index = state_->free_indices[state_->free_count - 1];
        if (state_->next_id ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error(
                "continuation ticket identity exhausted");

        const auto id = state_->next_id++;
        --state_->free_count;
        auto& slot = state_->slots[index];
        slot.id = id;
        slot.generation = state_->generation;
        slot.active = true;
        return Ticket(state_, index, id, state_->generation);
    }

    Execution try_claim(const Ticket& ticket) {
        auto owner = ticket.state_.lock();
        if (!owner || owner.get() != state_.get())
            return {};

        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed ||
            ticket.index_ >= MaxTickets ||
            ticket.generation_ != state_->generation)
            return {};

        auto& slot = state_->slots[ticket.index_];
        if (!slot.active ||
            slot.id != ticket.id_ ||
            slot.generation != ticket.generation_)
            return {};

        clear_slot(*state_, ticket.index_);
        ++state_->executing;
        return Execution(state_, ticket.generation_);
    }

    bool advance(std::uint64_t expected_generation) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed ||
            expected_generation != state_->generation ||
            state_->executing != 0)
            return false;
        if (state_->generation ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor generation exhausted");

        ++state_->generation;
        clear_pending(*state_);
        return true;
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
