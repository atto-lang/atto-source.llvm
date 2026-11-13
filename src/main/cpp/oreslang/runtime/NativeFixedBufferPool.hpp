#pragma once

// Trusted native runtime primitive. Not a guest-visible pointer or a source
// language implementation. Actors must be authenticated by the scheduler;
// passing arbitrary actor_id values does NOT establish authorization.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// A value-key only: the trusted scheduler must authenticate the caller.
// Never accept a key assembled from untrusted guest-provided IDs.
struct BufferActorIdentity final {
    std::uint64_t tenant_id;
    std::uint64_t actor_id;
    std::uint64_t generation;
    constexpr explicit operator bool() const noexcept {
        return tenant_id != 0 && actor_id != 0 && generation != 0;
    }
    constexpr bool operator==(const BufferActorIdentity&) const noexcept = default;
};

class NativeFixedBufferPool final {
    struct Slot {
        std::uint64_t generation = 1;
        BufferActorIdentity owner{0, 0, 0};
        bool leased = false;
        bool escrow = false; // sender capability is inaccessible during admission
        bool disabled = false;
    };
    struct State {
        State(std::size_t count, std::size_t width)
            : slot_bytes(width), bytes(count * width, 0), slots(count) {}
        std::mutex mutex;
        const std::size_t slot_bytes;
        std::vector<std::uint8_t> bytes;
        std::vector<Slot> slots;
        std::size_t in_use = 0;
        bool retired = false;
    };

public:
    class Lease final {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept { move_from(other); }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                move_from(other);
            }
            return *this;
        }
        ~Lease() { release(); }

        // No direct transfer: only try_prepare_transfer consumes the sender
        // handle, and PendingTransfer::commit mints the receiver handle.
        // This prevents a sender from releasing a successful transfer.

        // Scoped views are valid ONLY for the duration of this synchronous
        // callback. Never suspend, reenter the pool, or capture the span.
        // This does not implement an async-I/O pin or shared-memory authority.
        template <typename F>
        bool with_mutable_bytes(BufferActorIdentity actor, F&& callback) {
            if (!state_) return false;
            std::lock_guard<std::mutex> lock(state_->mutex);
            Slot& slot = state_->slots[index_];
            if (state_->retired || !valid(slot) || slot.escrow || slot.owner != actor)
                return false;
            callback(std::span<std::uint8_t>(
                state_->bytes.data() + index_ * state_->slot_bytes,
                state_->slot_bytes));
            return true;
        }

        template <typename F>
        bool with_read_only_bytes(BufferActorIdentity actor, F&& callback) const {
            if (!state_) return false;
            std::lock_guard<std::mutex> lock(state_->mutex);
            const Slot& slot = state_->slots[index_];
            if (state_->retired || !valid(slot) || slot.escrow || slot.owner != actor)
                return false;
            callback(std::span<const std::uint8_t>(
                state_->bytes.data() + index_ * state_->slot_bytes,
                state_->slot_bytes));
            return true;
        }

        bool release() noexcept {
            if (!state_) return false;
            // An in-flight escrow cannot be released through its Lease.
            // Its pending receipt must commit, abort, or explicitly discard.
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->slots[index_].escrow) return false;
            }
            // Keep State alive until AFTER the guard releases the mutex, even
            // when the pool is already destroyed and this is its final lease.
            auto state = std::move(state_);
            bool success = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                Slot& slot = state->slots[index_];
                success = valid(slot);
                if (success) {
                    // Sensitive data from the prior tenant/actor is never reused.
                    std::fill_n(state->bytes.data() + index_ * state->slot_bytes,
                                state->slot_bytes, std::uint8_t{0});
                    slot.leased = false;
                    slot.owner = {0, 0, 0};
                    if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
                        slot.disabled = true; // fail closed; never wrap a generation
                    } else {
                        ++slot.generation;
                    }
                    --state->in_use;
                }
            }
            return success;
        }

    private:
        friend class NativeFixedBufferPool;
        friend class PendingTransfer;
        Lease(std::shared_ptr<State> state, std::size_t index,
              std::uint64_t generation) noexcept
            : state_(std::move(state)), index_(index), generation_(generation) {}
        bool valid(const Slot& slot) const noexcept {
            return slot.leased && !slot.disabled && slot.generation == generation_;
        }
        void move_from(Lease& other) noexcept {
            state_ = std::move(other.state_);
            index_ = other.index_;
            generation_ = other.generation_;
            other.index_ = 0;
            other.generation_ = 0;
        }
        std::shared_ptr<State> state_;
        std::size_t index_ = 0;
        std::uint64_t generation_ = 0;
    };

    // Owns the unique sender lease during mailbox admission. Destroying an
    // unsettled escrow discards it safely; explicit abort restores ownership.
    // Only trusted host scheduler code may drive these methods. No guest access.
    class PendingTransfer final {
    public:
        PendingTransfer(const PendingTransfer&) = delete;
        PendingTransfer& operator=(const PendingTransfer&) = delete;
        PendingTransfer(PendingTransfer&&) noexcept = default;
        PendingTransfer& operator=(PendingTransfer&&) = delete;
        ~PendingTransfer() { discard(); }

        std::optional<Lease> commit(BufferActorIdentity recipient_actor) noexcept {
            if (!held_ || !held_->state_ || !recipient_actor) return std::nullopt;
            const auto state = held_->state_;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                Slot& slot = state->slots[held_->index_];
                if (state->retired || !held_->valid(slot) || !slot.escrow ||
                    slot.owner != original_actor_ ||
                    recipient_actor.tenant_id != original_actor_.tenant_id)
                    return std::nullopt;
                slot.owner = recipient_actor;
                slot.escrow = false;
            }
            return take();
        }

        std::optional<Lease> abort() noexcept {
            if (!held_ || !held_->state_) return std::nullopt;
            const auto state = held_->state_;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                Slot& slot = state->slots[held_->index_];
                if (!held_->valid(slot) || !slot.escrow ||
                    slot.owner != original_actor_)
                    return std::nullopt;
                // Abort succeeds even after retirement; release remains legal.
                slot.escrow = false;
            }
            return take();
        }

        // Dropped or cancelled without an explicit abort: revoke and scrub,
        // never silently put the lease back in the original sender's hands.
        bool discard() noexcept {
            if (!held_ || !held_->state_) return false;
            const auto state = held_->state_;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                Slot& slot = state->slots[held_->index_];
                if (!held_->valid(slot) || !slot.escrow ||
                    slot.owner != original_actor_)
                    return false;
                slot.escrow = false;
            }
            const bool released = held_->release();
            held_.reset();
            return released;
        }

    private:
        friend class NativeFixedBufferPool;
        PendingTransfer(Lease&& owner, BufferActorIdentity source) noexcept
            : held_(std::move(owner)), original_actor_(source) {}
        std::optional<Lease> take() noexcept {
            auto moved = std::move(held_);
            held_.reset();
            return moved;
        }
        std::optional<Lease> held_;
        BufferActorIdentity original_actor_{0, 0, 0};
    };

    static constexpr std::size_t max_pool_bytes = 1024u * 1024u * 1024u;

    NativeFixedBufferPool(std::size_t slot_count, std::size_t slot_bytes) {
        if (slot_count == 0 || slot_bytes == 0 ||
            slot_bytes > max_pool_bytes ||
            slot_count > max_pool_bytes / slot_bytes)
            throw std::invalid_argument("invalid fixed buffer pool capacity");
        state_ = std::make_shared<State>(slot_count, slot_bytes);
    }
    NativeFixedBufferPool(const NativeFixedBufferPool&) = delete;
    NativeFixedBufferPool& operator=(const NativeFixedBufferPool&) = delete;
    ~NativeFixedBufferPool() { retire(); }

    // Preallocated slot metadata and storage: this is allocation-free once
    // the constructor has succeeded. On exhaustion return nullopt; never grow.
    std::optional<Lease> try_acquire(BufferActorIdentity actor) {
        if (!actor) return std::nullopt;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->retired) return std::nullopt;
        for (std::size_t i = 0; i < state_->slots.size(); ++i) {
            Slot& slot = state_->slots[i];
            if (slot.leased || slot.disabled) continue;
            slot.leased = true;
            slot.owner = actor;
            ++state_->in_use;
            return Lease(state_, i, slot.generation);
        }
        return std::nullopt;
    }

    // Two-phase admission. Success immediately invalidates the caller's
    // Lease object; commit returns a new exclusive receiver Lease. Failed
    // preparation leaves the exact sender Lease untouched. No heap allocation.
    std::optional<PendingTransfer> try_prepare_transfer(
        Lease& sender, BufferActorIdentity expected_actor) noexcept {
        if (!sender.state_ || sender.state_.get() != state_.get() ||
            !expected_actor) return std::nullopt;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->retired) return std::nullopt;
        Slot& slot = state_->slots[sender.index_];
        if (!sender.valid(slot) || slot.escrow ||
            slot.owner != expected_actor) return std::nullopt;
        slot.escrow = true;
        return PendingTransfer(std::move(sender), expected_actor);
    }

    void retire() noexcept {
        if (!state_) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->retired = true;
        // Outstanding move-only leases keep State allocated until their
        // destruction, preventing storage use-after-free on pool teardown.
    }
    std::size_t in_use() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->in_use;
    }
    std::size_t capacity() const noexcept { return state_->slots.size(); }
    std::size_t slot_bytes() const noexcept { return state_->slot_bytes; }

private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
