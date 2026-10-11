#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang {

/**
 * Trusted native scheduler-facing actor-wide lifetime admission.
 *
 * At most one HTTP request OR actor message handler is admitted for this
 * actor incarnation at any time. A suspended handler retains its admission
 * while relinquishing its physical execution carrier. No guest callback or
 * C ABI function executes while this class holds its bookkeeping mutex.
 *
 * This does not implement Oreslang guest HTTP dispatch or actor source codegen.
 */
class SerialActorLifetimeGate final {
public:
    enum class Kind : std::uint8_t { Http, Message };
    // Mirrors Java SerialActorAdmission.Lifecycle without storing a second
    // mutable state alongside active_/suspended_/stopped_.
    enum class Lifecycle : std::uint8_t { Idle, Running, Suspended, Stopped };
    enum class SubmitStatus : std::uint8_t {
        Admitted, Queued, Full, Stopped, Duplicate, Exhausted
    };
    struct Lease {
        std::uint64_t actor_id = 0;
        std::uint64_t incarnation = 0;
        std::uint64_t generation = 0;
        std::uint64_t event_id = 0;
        Kind kind = Kind::Message;
    };
    struct SubmitResult {
        SubmitStatus status;
        std::optional<Lease> lease;
    };
    struct QueuedEvent { Kind kind; std::uint64_t event_id; };
    struct FinishResult {
        bool accepted;
        std::optional<Lease> next;
        // Scheduler must complete each dropped request/message with a failure.
        std::deque<QueuedEvent> rejected;
    };
    struct StopResult {
        std::optional<Lease> revoked;
        std::deque<QueuedEvent> rejected;
    };

    explicit SerialActorLifetimeGate(
            std::uint64_t actor_id,
            std::uint64_t incarnation,
            std::size_t pending_capacity)
        : actor_id_(actor_id), incarnation_(incarnation),
          capacity_(pending_capacity) {
        if (actor_id == 0 || incarnation == 0
                || pending_capacity < 1 || pending_capacity > 4096) {
            throw std::invalid_argument("invalid serial actor admission identity/capacity");
        }
    }
    SerialActorLifetimeGate(const SerialActorLifetimeGate&) = delete;
    SerialActorLifetimeGate& operator=(const SerialActorLifetimeGate&) = delete;

    SubmitResult submit(Kind kind, std::uint64_t event_id) {
        std::lock_guard<std::mutex> guard(lock_);
        if (stopped_) return {SubmitStatus::Stopped, std::nullopt};
        if (event_id == 0 || contains_event_locked(event_id)) {
            return {SubmitStatus::Duplicate, std::nullopt};
        }
        if (!active_) {
            if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
                return {SubmitStatus::Exhausted, std::nullopt};
            }
            Lease next{actor_id_, incarnation_, ++generation_, event_id, kind};
            active_ = next;
            suspended_ = false;
            return {SubmitStatus::Admitted, next};
        }
        if (pending_.size() >= capacity_) {
            return {SubmitStatus::Full, std::nullopt};
        }
        pending_.push_back({kind, event_id});
        return {SubmitStatus::Queued, std::nullopt};
    }

    bool suspend(const Lease& lease) {
        std::lock_guard<std::mutex> guard(lock_);
        if (!matches_locked(lease) || suspended_) return false;
        suspended_ = true;
        return true;
    }

    bool resume(const Lease& lease) {
        std::lock_guard<std::mutex> guard(lock_);
        if (!matches_locked(lease) || !suspended_) return false;
        suspended_ = false;
        return true;
    }

    /** An outstanding lifetime reservation may be suspended. */
    bool admitted(const Lease& lease) const {
        std::lock_guard<std::mutex> guard(lock_);
        return matches_locked(lease);
    }

    /** Authorizes guest execution only for the current unsuspended lease.
     * The transport/scheduler must check this before any handler or resumed
     * continuation enters guest code. No guest-supplied ticket is trusted. */
    bool may_execute(const Lease& lease) const {
        std::lock_guard<std::mutex> guard(lock_);
        return matches_locked(lease) && !suspended_;
    }

    /** Finishes a normal/suspended/canceled handler, then promotes FIFO work. */
    FinishResult finish(const Lease& lease) {
        std::lock_guard<std::mutex> guard(lock_);
        if (!matches_locked(lease)) return {false, std::nullopt, {}};
        active_.reset();
        suspended_ = false;
        if (pending_.empty() || stopped_) return {true, std::nullopt, {}};
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            stopped_ = true; // fail closed rather than wrap a trusted token
            std::deque<QueuedEvent> rejected;
            rejected.swap(pending_);
            return {true, std::nullopt, std::move(rejected)};
        }
        const auto next = pending_.front();
        pending_.pop_front();
        Lease ticket{actor_id_, incarnation_, ++generation_, next.event_id, next.kind};
        active_ = ticket;
        return {true, ticket, {}};
    }

    bool cancel_pending(std::uint64_t event_id) {
        std::lock_guard<std::mutex> guard(lock_);
        const auto it = std::find_if(pending_.begin(), pending_.end(),
                [event_id](const Pending& p) { return p.event_id == event_id; });
        if (it == pending_.end()) return false;
        pending_.erase(it);
        return true;
    }

    /** Revokes all tickets and returns the pending identities so the
     * supervisor can fail waiting HTTP requests and mailbox deliveries.
     * Never invoke user callbacks while holding this bookkeeping lock. */
    StopResult stop() {
        std::lock_guard<std::mutex> guard(lock_);
        StopResult result;
        if (stopped_) return result;
        stopped_ = true;
        result.revoked = active_;
        active_.reset();
        suspended_ = false;
        result.rejected.swap(pending_);
        return result;
    }

    Lifecycle lifecycle() const {
        std::lock_guard<std::mutex> guard(lock_);
        if (stopped_) return Lifecycle::Stopped;
        if (!active_) return Lifecycle::Idle;
        return suspended_ ? Lifecycle::Suspended : Lifecycle::Running;
    }

    bool busy() const {
        std::lock_guard<std::mutex> guard(lock_);
        return active_.has_value();
    }
    std::size_t queued() const {
        std::lock_guard<std::mutex> guard(lock_);
        return pending_.size();
    }

private:
    using Pending = QueuedEvent;
    bool matches_locked(const Lease& lease) const noexcept {
        return !stopped_ && active_
                && active_->actor_id == lease.actor_id
                && active_->incarnation == lease.incarnation
                && active_->generation == lease.generation
                && active_->event_id == lease.event_id
                && active_->kind == lease.kind;
    }
    bool contains_event_locked(std::uint64_t event_id) const {
        if (active_ && active_->event_id == event_id) return true;
        return std::any_of(pending_.begin(), pending_.end(),
                [event_id](const Pending& p) { return p.event_id == event_id; });
    }
    mutable std::mutex lock_;
    const std::uint64_t actor_id_;
    const std::uint64_t incarnation_;
    const std::size_t capacity_;
    std::deque<Pending> pending_;
    std::optional<Lease> active_;
    std::uint64_t generation_ = 0;
    bool suspended_ = false;
    bool stopped_ = false;
};

} // namespace oreslang
