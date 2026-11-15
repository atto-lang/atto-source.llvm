#pragma once

#include "oreslang/runtime/ActorMailboxCompletion.hpp"
#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
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
    static_assert(std::is_move_constructible_v<T>, "actor payload must be movable");

    using Queue = PreallocatedSelectiveSlots<T>;
    struct State {
        explicit State(std::size_t frames, std::size_t bytes,
                       std::shared_ptr<ActorMailboxCompletion> ledger)
            : queue(frames), max_frames(frames), max_bytes(bytes), ledger(std::move(ledger)) {
            if (!frames || !bytes) throw std::invalid_argument("positive port limits required");
        }
        std::mutex mutex;
        // All queue node storage is preallocated during State construction.
        // The caller-held mutex protects the fixed-slot free/active lists;
        // checked-out frames keep their port credit until physical ACK.
        Queue queue;
        std::size_t max_frames, max_bytes;
        std::size_t used_frames = 0, used_bytes = 0;
        // Preparations reserve the same credits as queued/checked-out frames.
        // Close may seal new work but must not reclaim a preparing message.
        std::size_t pending_prepares = 0;
        std::uint64_t next_id = 1;
        std::shared_ptr<ActorMailboxCompletion> ledger;
        bool closed = false;
    };

public:
    class Delivery {
    public:
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&&) noexcept = default;
        // Overwriting a live delivery would orphan its unacknowledged
        // accounting receipt. Only move construction is safe.
        Delivery& operator=(Delivery&&) noexcept = delete;
        T& value() {
            if (acknowledged_ || !payload_) throw std::logic_error("stale owned actor payload");
            return *payload_;
        }
        const T& value() const {
            if (acknowledged_ || !payload_) throw std::logic_error("stale owned actor payload");
            return *payload_;
        }
        std::uint64_t id() const noexcept { return id_; }
        std::uint64_t sequence() const noexcept { return sequence_; }
        std::size_t charged_bytes() const noexcept { return bytes_; }

        /** Release credit ONLY after all downstream processing and device
         * ownership/borrow fences have acknowledged quiescence. */
        bool acknowledge_released() {
            if (acknowledged_ || !state_) return false;
            // Terminalize the receipt before invoking potentially reentrant
            // payload cleanup. Its bytes stay charged until destructor and
            // external release hooks finish; never run them under our mutex.
            acknowledged_ = true;
            payload_.reset();
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!state_->used_frames || state_->used_bytes < bytes_)
                throw std::logic_error("actor port accounting corrupted");
            --state_->used_frames;
            state_->used_bytes -= bytes_;
            return true;
        }

        // Destruction of an unacknowledged delivery NEVER returns credits.
        // This fail-closed behavior prevents use-after-release on devices.
        ~Delivery() = default;
    private:
        friend class BoundedActorPort<T>;
        Delivery(std::shared_ptr<State> state, typename Queue::Frame envelope)
            : state_(std::move(state)), id_(envelope.id),
              bytes_(envelope.bytes), payload_(std::move(envelope.payload)),
              sequence_(envelope.sequence) {}
        std::shared_ptr<State> state_;
        std::uint64_t id_;
        std::size_t bytes_;
        std::unique_ptr<T> payload_;
        bool acknowledged_ = false;
        std::uint64_t sequence_ = 0;
    };

    BoundedActorPort(std::size_t max_frames, std::size_t max_bytes,
                     std::shared_ptr<ActorMailboxCompletion> ledger = {})
        : state_(std::make_shared<State>(max_frames, max_bytes, std::move(ledger))) {}

    BoundedActorPort(const BoundedActorPort&) = delete;
    BoundedActorPort& operator=(const BoundedActorPort&) = delete;

    // Nonzero tags are trusted runtime metadata, never guest-controlled
    // authority or an RTTI/reflection lookup. Legacy untagged traffic is 0.
    bool offer(T&& payload, std::size_t bytes, std::uint32_t match_tag = 0) {
        if (!bytes) throw std::invalid_argument("actor port frame charge must be positive");
        const auto state = state_;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            if (state->closed) throw std::logic_error("actor port closed");
            // Leave an ID for every concurrently preparing frame. Identity is
            // assigned on publish; rejected/throwing preparations consume none.
            if (state->next_id == 0 ||
                state->pending_prepares >
                    std::numeric_limits<std::uint64_t>::max() - state->next_id)
                throw std::overflow_error("actor frame identity exhausted");
            if (state->used_frames >= state->max_frames ||
                bytes > state->max_bytes - state->used_bytes)
                return false;
            // Hold the same window credit through preparation, publication,
            // queued delivery, and physical ACK. This rejects admission before
            // moving the caller's T when an earlier receipt stalls the window.
            if (state->ledger && !state->ledger->reserve_prepare()) return false;
            ++state->used_frames;
            state->used_bytes += bytes;
            ++state->pending_prepares;
        }

        // Construct user-owned T OUTSIDE the mailbox mutex. Its move
        // constructor may invoke FFI, acquire another actor lock or re-enter
        // this port. The reservation still enforces backpressure throughout.
        struct Reservation {
            std::shared_ptr<State> state;
            std::size_t bytes;
            bool committed = false;
            ~Reservation() noexcept {
                if (committed) return;
                std::lock_guard<std::mutex> guard(state->mutex);
                if (!state->pending_prepares || !state->used_frames ||
                    state->used_bytes < bytes) std::terminate();
                --state->pending_prepares;
                --state->used_frames;
                state->used_bytes -= bytes;
                if (state->ledger) state->ledger->cancel_prepare();
            }
        } reservation{state, bytes};

        auto owned = std::make_unique<T>(std::move(payload));
        // No queue allocation or T move after taking the physical mutex.
        // All user-defined move hooks have completed outside the lock.
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            // A concurrently running close() can seal new admissions while
            // a previously reserved frame finishes preparing. This reservation
            // already owns its credits, so it must be allowed to publish.
            if (state->next_id == 0)
                throw std::overflow_error("actor frame identity exhausted");
            // Queue publication is deterministic and allocation-free. A
            // failed invariant leaves 'owned' with the local sender while
            // Reservation rolls back byte/frame/ledger preparation credits.
            if (state->ledger) {
                state->ledger->publish([&](std::uint64_t sequence) {
                    if (!state->queue.push(owned, bytes, match_tag,
                                          sequence, state->next_id))
                        throw std::logic_error("reserved actor mailbox slot unavailable");
                });
            } else if (!state->queue.push(owned, bytes, match_tag,
                                          state->next_id, state->next_id)) {
                throw std::logic_error("reserved actor mailbox slot unavailable");
            }
            ++state->next_id;
            --state->pending_prepares;
            reservation.committed = true;
        }
        return true;
    }

    /**
     * Exception-safe exclusive capability admission. A sender holding a
     * unique_ptr<T> retains it on rejected admission OR queue allocation
     * failure. Unlike offer(T&&), no T move constructor runs and nothing
     * owns the underlying kernel/arena capability until after the queue
     * successfully publishes its preallocated empty envelope.
     *
     * This transport helper is NOT proof that the pointer's target is
     * shareable; caller must validate actor generation/handle rights first.
     */
    bool offer_owned(std::unique_ptr<T>& payload, std::size_t bytes,
                     std::uint32_t match_tag = 0) {
        if (!payload) throw std::invalid_argument("null owned actor payload");
        if (!bytes) throw std::invalid_argument("actor port frame charge must be positive");
        const auto state = state_;
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->closed) throw std::logic_error("actor port closed");
        if (state->next_id == 0 ||
            state->pending_prepares >
                std::numeric_limits<std::uint64_t>::max() - state->next_id)
            throw std::overflow_error("actor frame identity exhausted");
        if (state->used_frames >= state->max_frames ||
            bytes > state->max_bytes - state->used_bytes)
            return false;
        if (state->ledger && !state->ledger->reserve_prepare()) return false;
        ++state->used_frames;
        state->used_bytes += bytes;
        ++state->pending_prepares;

        try {
            if (state->ledger) {
                state->ledger->publish([&](std::uint64_t sequence) {
                    if (!state->queue.push(payload, bytes, match_tag,
                                           sequence, state->next_id))
                        throw std::logic_error("reserved actor mailbox slot unavailable");
                });
            } else if (!state->queue.push(payload, bytes, match_tag,
                                          state->next_id, state->next_id)) {
                throw std::logic_error("reserved actor mailbox slot unavailable");
            }
            // The producer's unique owner moves only at successful
            // publication; no user allocation occurs under this mutex.
            ++state->next_id;
            --state->pending_prepares;
            return true;
        } catch (...) {
            // Failed validation/publication never steals the producer's
            // owner and cannot consume a global ledger sequence.
            --state->pending_prepares;
            --state->used_frames;
            state->used_bytes -= bytes;
            if (state->ledger) state->ledger->cancel_prepare();
            throw;
        }
    }

    std::optional<Delivery> try_receive() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        auto frame = state_->queue.pop_front();
        if (!frame) return std::nullopt;
        return std::optional<Delivery>(Delivery(state_, std::move(*frame)));
    }

    // Stage-1 AOT immutable-tag selection. The preallocated queue owns its
    // slot identity and validates resumable cursors against slot generations.
    // No dynamic allocation or arbitrary guest matcher executes under lock.
    // At most scan_budget tags are examined; middle unlink is O(1).
    // This remains lane-local; Erlang global selective ordering, timeout/
    // guard lowering and hard-RT WCET qualification are separate contracts.
    using SelectiveStatus = typename Queue::Status;
    using SelectiveCursor = typename Queue::Cursor;
    struct SelectiveResult {
        SelectiveStatus status;
        std::optional<Delivery> delivery;
        std::size_t inspected;
        SelectiveCursor cursor;
    };

    SelectiveResult try_receive_tagged(
            std::uint32_t tag, std::size_t scan_budget,
            const SelectiveCursor& cursor = {}) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        auto result = state_->queue.select(tag, scan_budget, cursor);
        if (result.frame) {
            return {result.status,
                    std::optional<Delivery>(
                        Delivery(state_, std::move(*result.frame))),
                    result.inspected, std::move(result.cursor)};
        }
        return {result.status, std::nullopt, result.inspected,
                std::move(result.cursor)};
    }

    std::size_t used_frames() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->used_frames;
    }
    std::size_t used_bytes() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->used_bytes;
    }

    // Terminal retirement must not claim it drained this inbox while a
    // previously admitted producer is still preparing a publish. In
    // particular, close() seals admission but does NOT cancel in-flight
    // preparation or prevent the reserved envelope from being committed.
    std::size_t pending_prepares() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->pending_prepares;
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
