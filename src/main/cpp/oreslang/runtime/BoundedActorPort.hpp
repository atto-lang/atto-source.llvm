#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Bounded, move-only actor port. Credits represent both queued and checked-out
 * frames; receiving a frame never refunds credit. Only the owning receiver
 * can acknowledge after its processing / device fence has quiesced.
 *
 * No blocking OS wait and no unbounded pending writers. The actor scheduler
 * should suspend producers on admission failure and re-admit them on a
 * later authorized CONTROL/mailbox turn after an acknowledgement.
 */
template <typename T>
class BoundedActorPort final {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "actor payload move construction must be noexcept for rollback-safe admission");

    struct Envelope {
        std::uint64_t id = 0;
        std::size_t bytes = 0;
        std::unique_ptr<T> payload;
    };
    struct State {
        explicit State(std::size_t frames, std::size_t bytes)
            : max_frames(frames), max_bytes(bytes) {
            if (!frames || !bytes) throw std::invalid_argument("positive port limits required");
        }
        std::mutex mutex;
        std::deque<Envelope> queue;
        std::size_t max_frames, max_bytes;
        std::size_t used_frames = 0, used_bytes = 0;
        std::uint64_t next_id = 1;
        bool closed = false;
    };

public:
    class Delivery {
    public:
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&&) noexcept = default;
        // Replacing a live delivery would destroy its payload while leaving
        // the original frame/byte credits charged forever. A delivery lease
        // may move only by construction; assignment is deliberately sealed.
        Delivery& operator=(Delivery&&) = delete;
        T& value() {
            if (acknowledged_ || !payload_) throw std::logic_error("stale owned actor payload");
            return *payload_;
        }
        const T& value() const {
            if (acknowledged_ || !payload_) throw std::logic_error("stale owned actor payload");
            return *payload_;
        }
        std::uint64_t id() const noexcept { return id_; }
        std::size_t charged_bytes() const noexcept { return bytes_; }

        /** Release credit ONLY after all downstream processing and device
         * ownership/borrow fences have acknowledged quiescence. */
        bool acknowledge_released() {
            if (acknowledged_ || !state_) return false;
            std::unique_ptr<T> released;
            {
                std::lock_guard<std::mutex> guard(state_->mutex);
                if (!state_->used_frames || state_->used_bytes < bytes_)
                    throw std::logic_error("actor port accounting corrupted");
                acknowledged_ = true;
                --state_->used_frames;
                state_->used_bytes -= bytes_;
                released = std::move(payload_);
            }
            // User payload destruction is outside the admission/accounting
            // mutex. Destructors may run arbitrary cleanup and must not be
            // able to deadlock the port or stall unrelated producers.
            released.reset();
            return true;
        }

        // Destruction of an unacknowledged delivery NEVER returns credits.
        // This fail-closed behavior prevents use-after-release on devices.
        ~Delivery() = default;
    private:
        friend class BoundedActorPort<T>;
        Delivery(std::shared_ptr<State> state, Envelope envelope)
            : state_(std::move(state)), id_(envelope.id),
              bytes_(envelope.bytes), payload_(std::move(envelope.payload)) {}
        std::shared_ptr<State> state_;
        std::uint64_t id_;
        std::size_t bytes_;
        std::unique_ptr<T> payload_;
        bool acknowledged_ = false;
    };

    BoundedActorPort(std::size_t max_frames, std::size_t max_bytes)
        : state_(std::make_shared<State>(max_frames, max_bytes)) {}

    BoundedActorPort(const BoundedActorPort&) = delete;
    BoundedActorPort& operator=(const BoundedActorPort&) = delete;

    bool offer(T&& payload, std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("actor port frame charge must be positive");
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed) throw std::logic_error("actor port closed");
        if (state_->next_id == 0)
            throw std::overflow_error("actor frame identity exhausted");
        if (state_->used_frames >= state_->max_frames ||
            bytes > state_->max_bytes - state_->used_bytes)
            return false;
        // Reserve the deque node before touching caller ownership. Both the
        // deque growth and payload allocation may throw; neither is allowed
        // to consume a move-only sender value on failed admission.
        state_->queue.emplace_back();
        try {
            auto owned = std::make_unique<T>(std::move(payload));
            Envelope& admitted = state_->queue.back();
            admitted.id = state_->next_id;
            admitted.bytes = bytes;
            admitted.payload = std::move(owned);
        } catch (...) {
            state_->queue.pop_back();
            throw;
        }
        ++state_->next_id;
        ++state_->used_frames;
        state_->used_bytes += bytes;
        return true;
    }

    std::optional<Delivery> try_receive() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->queue.empty()) return std::nullopt;
        auto frame = std::move(state_->queue.front());
        state_->queue.pop_front();
        return std::optional<Delivery>(Delivery(state_, std::move(frame)));
    }

    std::size_t used_frames() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->used_frames;
    }
    std::size_t used_bytes() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->used_bytes;
    }

    void close() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->closed = true;
        if (state_->used_frames != 0)
            throw std::logic_error("actor port has unacknowledged owned frames");
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
