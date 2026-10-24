#pragma once
// Host-only scalar Future<T> precursor: guest lowering is not enabled.
// Result storage is a pinned shared allocation, never an actor heap pointer.
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

struct NativeScalarFutureId {
    std::uint64_t runtime_id;
    std::uint64_t actor_id;
    std::uint64_t generation;
    std::uint64_t future_id;
};

// No waiting on a carrier, no guest callback on a producer thread. The only
// executor of Resume is NativeActorGroup's single-flight parent CONTROL lane.
// The producer MUST retain a shared/weak frame reference until its callback
// quiesces. The owning supervisor MUST explicitly cancel on group teardown:
// the group can drop queued CONTROL callbacks without notifying this frame.
// This is not source Future<T> / await lowering.
class NativeScalarFutureAwait final
        : public std::enable_shared_from_this<NativeScalarFutureAwait> {
public:
    enum class Phase { pending, ready, scheduled, completed, cancelled };
    struct Result {
        bool success = false;
        std::int64_t value = 0;
        std::int64_t error_code = 0;
    };
    using Resume = std::function<void(const Result&)>;

    static std::shared_ptr<NativeScalarFutureAwait> open(
            const NativeActorRef& owner, std::uint64_t future_id) {
        if (!owner.runtime_id() || !owner.actor_id() || !owner.generation()
                || !future_id)
            throw std::invalid_argument("invalid scalar future owner identity");
        return std::shared_ptr<NativeScalarFutureAwait>(
            new NativeScalarFutureAwait({owner.runtime_id(), owner.actor_id(),
                                         owner.generation(), future_id}));
    }

    NativeScalarFutureAwait(const NativeScalarFutureAwait&) = delete;
    NativeScalarFutureAwait& operator=(const NativeScalarFutureAwait&) = delete;

    NativeScalarFutureId id() const noexcept { return id_; }

    bool complete(NativeScalarFutureId ticket, std::int64_t value) noexcept {
        return settle(ticket, Result{true, value, 0});
    }

    bool fail(NativeScalarFutureId ticket, std::int64_t error_code) noexcept {
        if (error_code <= 0) return false;
        return settle(ticket, Result{false, 0, error_code});
    }

    // Pending/ready/queued cancellation suppresses any later CONTROL callback.
    // Once the callback has started, it is completed and cannot be revoked.
    bool cancel(NativeScalarFutureId ticket) noexcept {
        if (!valid(ticket)) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ == Phase::completed || phase_ == Phase::cancelled) return false;
        phase_ = Phase::cancelled;
        return true;
    }

    Phase phase() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return phase_;
    }

    // Parent scheduler only. If CONTROL is full, leave the resolved value
    // intact for a later retry. Actor IDs AND generation are authenticated.
    bool try_schedule(NativeScalarFutureId ticket,
                      NativeActorGroup& parent_group,
                      const NativeActorRef& parent_ref,
                      Resume resume) {
        if (!resume) throw std::invalid_argument("missing scalar future resume");
        if (!valid(ticket) || parent_ref.runtime_id() != id_.runtime_id
                || parent_ref.actor_id() != id_.actor_id
                || parent_ref.generation() != id_.generation)
            return false;

        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::ready) return false;
        auto self = shared_from_this();
        if (!parent_group.enqueue_control(parent_ref,
                [self, resume = std::move(resume)] {
                    Result outcome;
                    {
                        std::lock_guard<std::mutex> lock(self->mutex_);
                        if (self->phase_ != Phase::scheduled) return;
                        outcome = self->result_;
                        self->phase_ = Phase::completed;
                    }
                    resume(outcome);
                }))
            return false;
        phase_ = Phase::scheduled;
        return true;
    }

private:
    explicit NativeScalarFutureAwait(NativeScalarFutureId id) : id_(id) {}

    bool valid(NativeScalarFutureId ticket) const noexcept {
        return ticket.runtime_id == id_.runtime_id
            && ticket.actor_id == id_.actor_id
            && ticket.generation == id_.generation
            && ticket.future_id == id_.future_id;
    }

    bool settle(NativeScalarFutureId ticket, Result outcome) noexcept {
        if (!valid(ticket)) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::pending) return false;
        result_ = outcome;
        phase_ = Phase::ready;
        return true;
    }

    const NativeScalarFutureId id_;
    mutable std::mutex mutex_;
    Phase phase_ = Phase::pending;
    Result result_;
};

} // namespace oreslang::runtime
