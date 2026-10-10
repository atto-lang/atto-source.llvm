#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Native CONTROL-plane substrate only. A handle owns no actor heap and has
 * no permission to dereference actor memory; it retains a small, shared
 * readiness ticket so awaiters can observe startup failure after retirement.
 * Source-level spawn/await lowering is intentionally NOT implemented here.
 */
struct ActorReadyResult {
    bool ready = false;
    std::string error;
};

class NativeActorRef final {
public:
    using ReadyContinuation = std::function<void(const ActorReadyResult&)>;

    std::uint64_t runtime_id() const noexcept { return runtime_id_; }
    std::uint64_t actor_id() const noexcept { return actor_id_; }
    std::uint64_t generation() const noexcept { return generation_; }

    bool operator==(const NativeActorRef& other) const noexcept {
        return runtime_id_ == other.runtime_id_
            && actor_id_ == other.actor_id_
            && generation_ == other.generation_;
    }

    /**
     * Register an enqueue-only host continuation. Never run guest code from
     * this callback; the caller must post guest resumption to its scheduler.
     * May be called after actor termination without resurrecting the actor.
     */
    void when_ready(ReadyContinuation cb) const {
        if (!cb) throw std::invalid_argument("null readiness continuation");
        ActorReadyResult result;
        {
            std::lock_guard<std::mutex> guard(ticket_->mutex);
            if (!ticket_->settled) {
                ticket_->waiters.push_back(std::move(cb));
                return;
            }
            result = ticket_->result;
        }
        cb(result);
    }

private:
    struct Ticket {
        std::mutex mutex;
        bool settled = false;
        ActorReadyResult result;
        std::vector<ReadyContinuation> waiters;
    };

    friend class NativeActorGroup;

    NativeActorRef(std::uint64_t runtime_id, std::uint64_t actor_id,
                   std::uint64_t generation, std::shared_ptr<Ticket> ticket)
        : runtime_id_(runtime_id), actor_id_(actor_id),
          generation_(generation), ticket_(std::move(ticket)) {}

    std::uint64_t runtime_id_;
    std::uint64_t actor_id_;
    std::uint64_t generation_;
    std::shared_ptr<Ticket> ticket_;
};

/**
 * Bounded, group-owned worker references. Membership is not ownership:
 * only explicitly adopted workers are stopped on group close. No native
 * collector or actor scheduler is embedded in this class.
 */
class NativeActorGroup final {
public:
    using StopRequest = std::function<void()>;

    explicit NativeActorGroup(std::uint64_t runtime_id, std::size_t maximum_workers)
        : runtime_id_(runtime_id), maximum_workers_(maximum_workers) {
        if (runtime_id == 0 || maximum_workers == 0)
            throw std::invalid_argument("invalid ActorGroup runtime/capacity");
    }

    NativeActorGroup(const NativeActorGroup&) = delete;
    NativeActorGroup& operator=(const NativeActorGroup&) = delete;

    ~NativeActorGroup() { close(); }

    /** Immediate native equivalent of spawn: not a readiness barrier. */
    NativeActorRef adopt(std::uint64_t runtime_id, std::uint64_t actor_id,
                         std::uint64_t generation, StopRequest stop_request) {
        if (!stop_request || actor_id == 0 || generation == 0)
            throw std::invalid_argument("invalid native actor admission");
        if (runtime_id != runtime_id_)
            throw std::invalid_argument("foreign runtime actor reference");
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("ActorGroup is closed");
        if (workers_.find(actor_id) != workers_.end())
            throw std::logic_error("actor already managed by group");
        if (workers_.size() >= maximum_workers_)
            throw std::logic_error("ActorGroup worker capacity exceeded");
        auto ticket = std::make_shared<NativeActorRef::Ticket>();
        NativeActorRef ref(runtime_id, actor_id, generation, std::move(ticket));
        workers_.emplace(actor_id, Entry{ref, std::move(stop_request), Phase::starting});
        return ref;
    }

    /**
     * Successful startup settles the readiness ticket; await spawn yields
     * its existing handle. The native executor/continuation scheduler must
     * invoke this on successful actor initialization.
     */
    bool mark_ready(const NativeActorRef& ref) {
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* entry = find_active_locked(ref);
            if (!entry || entry->phase != Phase::starting || closed_) return false;
            entry->phase = Phase::ready;
            callbacks = settle_ticket(ref, {true, ""});
        }
        notify(callbacks, {true, ""});
        return true;
    }

    /**
     * Failure before ready always rejects the ticket, even when no awaiter
     * has been registered yet. Reference-counted tickets outlive the table.
     */
    bool retire(const NativeActorRef& ref, std::string error = "actor terminated before ready") {
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        ActorReadyResult result{false, std::move(error)};
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* entry = find_active_locked(ref);
            if (!entry) return false;
            callbacks = settle_ticket(ref, result);
            workers_.erase(ref.actor_id());
        }
        notify(callbacks, result);
        return true;
    }

    /** Request stop once for each managed actor, without blocking carriers. */
    void stop_workers() {
        std::vector<StopRequest> stops;
        std::vector<std::pair<std::vector<NativeActorRef::ReadyContinuation>,
                              ActorReadyResult>> notifications;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            for (auto& item : workers_) {
                Entry& worker = item.second;
                if (worker.phase == Phase::stopping) continue;
                if (worker.phase == Phase::starting) {
                    ActorReadyResult result{false, "worker stopped before ready"};
                    notifications.emplace_back(settle_ticket(worker.ref, result), result);
                }
                worker.phase = Phase::stopping;
                stops.push_back(worker.stop_request);
            }
        }
        for (auto& notification : notifications)
            notify(notification.first, notification.second);
        request_stops(stops);
    }

    /** Fence admissions and readiness before invoking any stop callback. */
    void close() noexcept {
        std::vector<StopRequest> stops;
        std::vector<std::pair<std::vector<NativeActorRef::ReadyContinuation>,
                              ActorReadyResult>> notifications;
        try {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) return;
                closed_ = true;
                for (auto& item : workers_) {
                    Entry& worker = item.second;
                    ActorReadyResult result{false, "ActorGroup closed before ready"};
                    notifications.emplace_back(settle_ticket(worker.ref, result), result);
                    if (worker.phase != Phase::stopping)
                        stops.push_back(std::move(worker.stop_request));
                }
                workers_.clear();
            }
            for (auto& notification : notifications)
                notify(notification.first, notification.second);
            request_stops(stops);
        } catch (...) {
            // Destructors cannot throw. Supervisor callbacks must be noexcept
            // or report failure through a separate host error channel.
        }
    }

    std::size_t worker_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return workers_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }

private:
    enum class Phase { starting, ready, stopping };
    struct Entry {
        NativeActorRef ref;
        StopRequest stop_request;
        Phase phase;
    };

    Entry* find_active_locked(const NativeActorRef& ref) {
        if (ref.runtime_id() != runtime_id_) return nullptr;
        const auto it = workers_.find(ref.actor_id());
        if (it == workers_.end() || !(it->second.ref == ref)) return nullptr;
        return &it->second;
    }

    static std::vector<NativeActorRef::ReadyContinuation> settle_ticket(
            const NativeActorRef& ref, const ActorReadyResult& result) {
        std::lock_guard<std::mutex> guard(ref.ticket_->mutex);
        if (ref.ticket_->settled) return {};
        ref.ticket_->settled = true;
        ref.ticket_->result = result;
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        callbacks.swap(ref.ticket_->waiters);
        return callbacks;
    }

    static void notify(
            const std::vector<NativeActorRef::ReadyContinuation>& callbacks,
            const ActorReadyResult& result) noexcept {
        for (const auto& callback : callbacks) {
            try { callback(result); } catch (...) {
                // Callback dispatch must not strand other waiters or workers.
            }
        }
    }

    static void request_stops(const std::vector<StopRequest>& stops) noexcept {
        for (const auto& stop : stops) {
            try { stop(); } catch (...) {
                // Host supervisors record/report callback failures separately.
            }
        }
    }

    const std::uint64_t runtime_id_;
    const std::size_t maximum_workers_;
    mutable std::mutex mutex_;
    bool closed_ = false;
    std::unordered_map<std::uint64_t, Entry> workers_;
};

}  // namespace oreslang::runtime
