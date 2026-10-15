#pragma once

#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
              capacity_(capacity) {
        if (supervisor_id_.empty()) {
            throw std::invalid_argument("CONTROL inbox requires supervisor identity");
        }
        if (capacity == 0 || capacity > 256) {
            throw std::invalid_argument("invalid bounded supervisor inbox capacity");
        }
    }

    ActorFailureControlInbox(const ActorFailureControlInbox&) = delete;
    ActorFailureControlInbox& operator=(const ActorFailureControlInbox&) = delete;

    bool offer(const ActorFailureControlNotice& notice) {
        if (notice.envelope().supervisor_id() != supervisor_id_) {
            throw std::invalid_argument("CONTROL notice is not owned by this supervisor");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.size() >= capacity_) {
            ++dropped_;
            return false;
        }
        pending_.push_back(notice);
        return true;
    }

    std::optional<ActorFailureControlNotice> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) return std::nullopt;
        auto next = std::move(pending_.front());
        pending_.pop_front();
        return next;
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

private:
    const std::string supervisor_id_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<ActorFailureControlNotice> pending_;
    std::size_t dropped_ = 0;
};

}  // namespace oreslang::runtime
