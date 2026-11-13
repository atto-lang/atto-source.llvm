#pragma once

#include "oreslang/runtime/NativeFixedBufferPool.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Preallocated HOST data lane for transferable pool buffers; NOT an actor
// scheduler, CONTROL lane, authenticated guest capability, or I/O pin. A
// trusted actor turn and resource-registry lifecycle fence are still required.
// Credits stay charged until explicit physical release, not dequeue.
class NativeFixedBufferMailbox final {
    using Lease = NativeFixedBufferPool::Lease;
    struct State {
        explicit State(std::size_t slots, std::uint64_t receiver)
            : target_actor(receiver), ring(slots) {}
        std::mutex mutex;
        const std::uint64_t target_actor;
        std::vector<std::optional<Lease>> ring;
        std::size_t head = 0, tail = 0, queued = 0, charged = 0;
        bool retired = false;
    };

public:
    class Delivery final {
    public:
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&&) noexcept = default;
        Delivery& operator=(Delivery&&) = delete;
        ~Delivery() = default;

        Lease& lease() { return *owned_; }
        bool acknowledge_released() noexcept {
            if (!owned_ || !state_ || !owned_->release()) return false;
            owned_.reset();
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->charged == 0) std::terminate();
            --state_->charged;
            return true;
        }

    private:
        friend class NativeFixedBufferMailbox;
        Delivery(std::shared_ptr<State> state, Lease&& lease) noexcept
            : state_(std::move(state)), owned_(std::move(lease)) {}
        std::shared_ptr<State> state_;
        std::optional<Lease> owned_;
    };

    NativeFixedBufferMailbox(std::size_t slots, std::uint64_t target_actor) {
        if (slots == 0 || target_actor == 0 || slots > (1u << 20))
            throw std::invalid_argument("invalid native buffer mailbox size/target");
        state_ = std::make_shared<State>(slots, target_actor);
    }
    NativeFixedBufferMailbox(const NativeFixedBufferMailbox&) = delete;
    NativeFixedBufferMailbox& operator=(const NativeFixedBufferMailbox&) = delete;
    ~NativeFixedBufferMailbox() { retire(); }

    // Caller must be an authenticated trusted host scheduler. An unsuccessful
    // offer does not consume the sender's lease; successful offer does.
    // Slot reservation and transfer commit occur under the same queue lock,
    // so no receiver may observe a partially published lease.
    bool try_offer(NativeFixedBufferPool& pool, Lease& sender,
                   std::uint64_t source_actor) noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->retired || state_->charged == state_->ring.size()) return false;
        auto prepared = pool.try_prepare_transfer(sender, source_actor);
        if (!prepared) return false;
        auto delivered = prepared->commit(state_->target_actor);
        if (!delivered) {
            auto restored = prepared->abort();
            if (restored) sender = std::move(*restored);
            // A failed restoration is fail-closed: the pending receipt's
            // destructor discards the payload, never publishes a partial send.
            return false;
        }
        state_->ring[state_->tail].emplace(std::move(*delivered));
        state_->tail = (state_->tail + 1) % state_->ring.size();
        ++state_->queued;
        ++state_->charged;
        return true;
    }

    std::optional<Delivery> try_receive() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->queued == 0) return std::nullopt;
        auto lease = std::move(*state_->ring[state_->head]);
        state_->ring[state_->head].reset();
        state_->head = (state_->head + 1) % state_->ring.size();
        --state_->queued;
        return Delivery(state_, std::move(lease));
    }

    // Seal new sends, but allow receiver to settle already charged deliveries.
    // False means the scheduler MUST retain this mailbox until quiescence.
    bool retire() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->retired = true;
        return state_->charged == 0;
    }
    std::size_t charged() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->charged;
    }
    std::size_t queued() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->queued;
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
