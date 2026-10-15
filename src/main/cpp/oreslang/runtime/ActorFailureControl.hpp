#pragma once

#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Host-only CONTROL metadata until native guest exception/unwind lowering exists.
// Never accept an exception_ptr, actor heap handle, raw Error or guest diagnostic.
enum class ActorExceptionalEffect : std::uint8_t { Raise, Panic };

class ActorFailureControlNotice {
public:
    static ActorFailureControlNotice from_guest_effect(
            ActorExceptionalEffect effect,
            std::string_view actor_id,
            std::string_view supervisor_id) {
        // Generic catalog envelopes also represent process errors and permit
        // an empty identity. A terminal actor CONTROL notice must not.
        if (actor_id.empty()) {
            throw std::invalid_argument("actor CONTROL notice requires actor identity");
        }
        switch (effect) {
            case ActorExceptionalEffect::Raise:
                return ActorFailureControlNotice(effect,
                        ActorErrorEnvelope::make(ActorErrorCode::ActorSupervision,
                                "actor raised a typed Error", actor_id, supervisor_id));
            case ActorExceptionalEffect::Panic:
                return ActorFailureControlNotice(effect,
                        ActorErrorEnvelope::make(ActorErrorCode::ActorSupervision,
                                "actor panicked with a typed Error", actor_id, supervisor_id));
        }
        throw std::invalid_argument("invalid actor exceptional effect");
    }

    ActorExceptionalEffect effect() const noexcept { return effect_; }
    const ActorErrorEnvelope& envelope() const noexcept { return envelope_; }

private:
    ActorFailureControlNotice(ActorExceptionalEffect effect,
                              ActorErrorEnvelope envelope)
            : effect_(effect), envelope_(std::move(envelope)) {}

    ActorExceptionalEffect effect_;
    ActorErrorEnvelope envelope_;
};

/**
 * Bounded, host-only inbox for direct-child terminal CONTROL notices.
 *
 * The owner is the runtime-chosen structured supervisor, not a guest-supplied
 * address. The eventual native scheduler must own and access this object; it
 * is deliberately not a guest mailbox, language Error or actor callback.
 *
 * No error object, stack, exception_ptr or executable pointer is retained.
 */
class ActorFailureControlInbox final {
public:
    explicit ActorFailureControlInbox(
            std::string_view supervisor_id,
            std::size_t capacity = 256)
            : supervisor_id_(checked_actor_identity(supervisor_id)),
              capacity_(checked_capacity(capacity)),
              slots_(capacity_) {
        if (supervisor_id_.empty()) {
            throw std::invalid_argument("CONTROL inbox requires supervisor identity");
        }
    }

    ActorFailureControlInbox(const ActorFailureControlInbox&) = delete;
    ActorFailureControlInbox& operator=(const ActorFailureControlInbox&) = delete;

    // The incoming notice is copied before acquiring the lock. Once inside,
    // only std::string moves and optional emplacement into fixed slots occur:
    // no queue growth, reallocations, guest callbacks or unbounded waits.
    bool offer(ActorFailureControlNotice notice) {
        if (notice.envelope().supervisor_id() != supervisor_id_) {
            throw std::invalid_argument("CONTROL notice is not owned by this supervisor");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_ || count_ == capacity_) {
            // Late notifications and full-ring overflow are equally visible
            // in the durable supervisor loss counter.
            ++dropped_;
            return false;
        }
        slots_[(head_ + count_) % capacity_].emplace(std::move(notice));
        ++count_;
        return true;
    }

    std::optional<ActorFailureControlNotice> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return std::nullopt;
        // Move into caller-owned metadata; never retain a pointer into a
        // supervisor-owned slot after releasing its lock.
        std::optional<ActorFailureControlNotice> result(
                std::move(*slots_[head_]));
        slots_[head_].reset();
        head_ = (head_ + 1) % capacity_;
        --count_;
        return result;
    }

    /**
     * Irrevocably close CONTROL admission. Retirement and offer serialize on
     * the same mutex: no late producer can revive a cleared supervisor inbox.
     * Count unread notices as dropped rather than destroying them silently.
     */
    void retire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        retired_ = true;
        dropped_ += count_;
        for (auto& slot : slots_) slot.reset();
        head_ = 0;
        count_ = 0;
    }

    bool retired() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return retired_;
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0 || capacity > 256) {
            throw std::invalid_argument("invalid bounded supervisor inbox capacity");
        }
        return capacity;
    }

    const std::string supervisor_id_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<std::optional<ActorFailureControlNotice>> slots_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    bool retired_ = false;
};

}  // namespace oreslang::runtime
