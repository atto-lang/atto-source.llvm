#pragma once

#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
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
        // A child can never be its own structured supervisor. Reject forged
        // self-parenting metadata before constructing the CONTROL envelope.
        if (actor_id == supervisor_id) {
            throw std::invalid_argument("actor CONTROL notice cannot supervise itself");
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
    // Zero means not admitted to a trusted supervisor CONTROL inbox.
    // These read-only values tag a delivered event; neither value confers
    // permission to publish a new event or revoke a child registration.
    std::uint64_t admitted_inbox_incarnation() const noexcept {
        return admitted_inbox_incarnation_;
    }
    std::uint64_t admitted_child_generation() const noexcept {
        return admitted_child_generation_;
    }

private:
    friend class ActorFailureControlInbox;
    ActorFailureControlNotice(ActorExceptionalEffect effect,
                              ActorErrorEnvelope envelope)
            : effect_(effect), envelope_(std::move(envelope)) {}

    ActorExceptionalEffect effect_;
    ActorErrorEnvelope envelope_;
    std::uint64_t admitted_inbox_incarnation_ = 0;
    std::uint64_t admitted_child_generation_ = 0;
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
              slots_(capacity_),
              inbox_nonce_(allocate_inbox_nonce()) {
        if (supervisor_id_.empty()) {
            throw std::invalid_argument("CONTROL inbox requires supervisor identity");
        }
    }

    ActorFailureControlInbox(const ActorFailureControlInbox&) = delete;
    ActorFailureControlInbox& operator=(const ActorFailureControlInbox&) = delete;

    // This is a host-issued registration capability, not a guest ActorRef.
    // It carries a unique inbox incarnation and a monotonic child generation.
    // Its constructor is private; the native runtime retains a copy at spawn.
    class DirectChildPermit final {
    public:
        DirectChildPermit(const DirectChildPermit&) = default;
        DirectChildPermit(DirectChildPermit&&) = default;
        DirectChildPermit& operator=(const DirectChildPermit&) = default;
        DirectChildPermit& operator=(DirectChildPermit&&) = default;
        std::string_view child_id() const noexcept { return child_id_; }
    private:
        friend class ActorFailureControlInbox;
        DirectChildPermit(std::string id, std::uint64_t owner, std::uint64_t generation)
            : child_id_(std::move(id)), inbox_nonce_(owner), generation_(generation) {}
        std::string child_id_;
        std::uint64_t inbox_nonce_;
        std::uint64_t generation_;
    };

    // Call ONLY from the host scheduler while admitting an actual structured
    // child. A notice, guest ActorRef, or source argument cannot create this
    // registration. Bounds are independent of the CONTROL notice ring.
    std::optional<DirectChildPermit> register_direct_child(std::string_view child_id) {
        const std::string checked = checked_actor_identity(child_id);
        if (checked.empty() || checked == supervisor_id_) {
            throw std::invalid_argument("invalid structured child identity");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return std::nullopt;
        if (std::any_of(direct_children_.begin(), direct_children_.end(),
                [&](const DirectChild& entry) { return entry.id == checked; })
                || direct_children_.size() == max_direct_children) return std::nullopt;
        if (next_child_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("native child generation counter exhausted");
        }
        const std::uint64_t generation = next_child_generation_++;
        DirectChildPermit permit(checked, inbox_nonce_, generation);
        direct_children_.push_back({checked, generation, false});
        return permit;
    }

    // Revocation must use the original generation's permit. A late teardown
    // cannot revoke a new child that reuses the same string identity.
    bool unregister_direct_child(const DirectChildPermit& permit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (permit.inbox_nonce_ != inbox_nonce_) return false;
        auto found = std::find_if(direct_children_.begin(), direct_children_.end(),
                [&](const DirectChild& entry) {
                    return entry.id == permit.child_id_
                            && entry.generation == permit.generation_;
                });
        if (found == direct_children_.end()) return false;
        *found = std::move(direct_children_.back());
        direct_children_.pop_back();
        return true;
    }

    // Native actor spawn must supply the exact trusted registration permit;
    // matching strings alone cannot authorize a CONTROL effect.
    // The incoming notice is copied before acquiring the lock. Once inside,
    // only std::string moves and optional emplacement into fixed slots occur:
    // no queue growth, reallocations, guest callbacks or unbounded waits.
    bool offer(ActorFailureControlNotice notice, const DirectChildPermit& permit) {
        // A prior delivery cannot be recycled into a new authoritative
        // publication. The actor/runtime must construct fresh, unadmitted
        // value metadata for each terminal effect.
        if (notice.admitted_inbox_incarnation_ != 0
                || notice.admitted_child_generation_ != 0) {
            throw std::invalid_argument("CONTROL notice already admitted by an inbox");
        }
        if (notice.envelope().supervisor_id() != supervisor_id_
                || permit.inbox_nonce_ != inbox_nonce_) {
            throw std::invalid_argument("CONTROL notice is not owned by this supervisor");
        }
        if (permit.child_id_.empty()
                || permit.child_id_ == supervisor_id_
                || notice.envelope().actor_id() != permit.child_id_) {
            throw std::invalid_argument("CONTROL notice does not match direct-child authority");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) {
            // Late notices after retirement cannot revive child authority.
            dropped_ = saturated_control_count(dropped_, 1);
            return false;
        }
        auto child = std::find_if(direct_children_.begin(), direct_children_.end(),
                [&](const DirectChild& entry) {
                    return entry.id == permit.child_id_
                            && entry.generation == permit.generation_;
                });
        if (child == direct_children_.end()) {
            throw std::invalid_argument("CONTROL notice requires registered direct child");
        }
        if (child->terminal_admitted) {
            // A terminal actor outcome can never be delivered twice to its
            // supervisor, even after its first notice has been consumed.
            // Replay rejection is not a lost first notice.
            duplicate_rejections_ = saturated_control_count(duplicate_rejections_, 1);
            return false;
        }
        if (count_ == capacity_) {
            dropped_ = saturated_control_count(dropped_, 1);
            return false;
        }
        // Stamp the accepted generation only after all validation and quota
        // decisions. These two scalar fields are host-owned data, never a
        // capability and never a borrowed actor/guest object.
        notice.admitted_inbox_incarnation_ = inbox_nonce_;
        notice.admitted_child_generation_ = child->generation;
        slots_[(head_ + count_) % capacity_].emplace(std::move(notice));
        child->terminal_admitted = true;
        ++count_;
        return true;
    }

    // A supervisor deciding whether to restart a *current* child must not
    // mistake a queued terminal notice from a retired registration for a
    // replacement actor with the same opaque string ID.
    // This is an observational check, not authority to offer new CONTROL.
    bool belongs_to_current_child(
            const ActorFailureControlNotice& notice,
            const DirectChildPermit& permit) const {
        if (permit.inbox_nonce_ != inbox_nonce_
                || notice.admitted_inbox_incarnation_ != inbox_nonce_
                || notice.admitted_child_generation_ != permit.generation_
                || notice.envelope().supervisor_id() != supervisor_id_
                || notice.envelope().actor_id() != permit.child_id_) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return false;
        return std::any_of(direct_children_.begin(), direct_children_.end(),
                [&](const DirectChild& entry) {
                    return entry.id == permit.child_id_
                            && entry.generation == permit.generation_;
                });
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
        dropped_ = saturated_control_count(dropped_, count_);
        for (auto& slot : slots_) slot.reset();
        direct_children_.clear();
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

    std::size_t duplicate_rejections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return duplicate_rejections_;
    }

private:
    struct DirectChild {
        std::string id;
        std::uint64_t generation;
        bool terminal_admitted = false;
    };
    static constexpr std::size_t max_direct_children = 4096;
    static inline std::atomic<std::uint64_t> next_inbox_nonce_{1};

    // Do not wrap: address-based identity permits ABA on placement-new reuse.
    static std::uint64_t allocate_inbox_nonce() {
        auto nonce = next_inbox_nonce_.load(std::memory_order_relaxed);
        for (;;) {
            if (nonce == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("native CONTROL inbox incarnations exhausted");
            }
            if (next_inbox_nonce_.compare_exchange_weak(
                    nonce, nonce + 1, std::memory_order_relaxed)) return nonce;
        }
    }

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
    // Host-admitted structural membership, never inferred from a notice.
    // Registration allocates only at actor spawn, never in offer/try_pop.
    std::vector<DirectChild> direct_children_;
    // Monotonic registration generations are never recycled during inbox life.
    const std::uint64_t inbox_nonce_;
    std::uint64_t next_child_generation_ = 1;
    // An entry stays terminal-fenced until exact-permit child unregister.
    std::size_t duplicate_rejections_ = 0;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    bool retired_ = false;
};

}  // namespace oreslang::runtime
