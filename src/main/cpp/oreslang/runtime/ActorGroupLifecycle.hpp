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
            callbacks = settle_ticket(entry->ref, {true, ""});
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
            callbacks = settle_ticket(entry->ref, result);
            workers_.erase(ref.actor_id());
        }
        notify(callbacks, result);
        return true;
    }

    /**
     * Request stop once for each managed actor without blocking a carrier.
     * Stage stop callbacks and notification capacity *before* changing
     * phases, so an allocation failure cannot strand a "stopping" worker
     * whose stop request was never actually dispatched.
     */
    void stop_workers() {
        std::vector<StopRequest> stops;
        std::vector<std::vector<NativeActorRef::ReadyContinuation>> waiters;
        const ActorReadyResult rejected{false, "worker stopped before ready"};
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            stops.reserve(workers_.size());
            waiters.reserve(workers_.size());
            for (const auto& item : workers_) {
                const Entry& worker = item.second;
                if (worker.phase == Phase::stopping) continue;
                stops.push_back(worker.stop_request); // may throw, nothing changed yet
            }
            // The fallible work above has completed; now gate readiness and
            // stop retries under the same group lock.
            for (auto& item : workers_) {
                Entry& worker = item.second;
                if (worker.phase == Phase::stopping) continue;
                if (worker.phase == Phase::starting) {
                    // Each output vector moves existing callbacks, no guest
                    // callback runs under the actor-group mutex.
                    waiters.push_back(settle_ticket(worker.ref, rejected));
                }
                worker.phase = Phase::stopping;
            }
        }
        for (auto& callbacks : waiters) notify(callbacks, rejected);
        request_stops(stops);
    }

    /**
     * Close never allocates to collect stop callbacks. Detach the entire
     * worker table with unordered_map::swap under the admission lock, then
     * process every detached entry independently. An exception from a
     * readiness callback can never prevent the corresponding stop request.
     */
    void close() noexcept {
        std::unordered_map<std::uint64_t, Entry> detached;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            closed_ = true;
            detached.swap(workers_);
        }
        for (auto& item : detached) {
            Entry& worker = item.second;
            if (worker.phase == Phase::starting) {
                try {
                    const ActorReadyResult rejected{false, "ActorGroup closed before ready"};
                    auto callbacks = settle_ticket(worker.ref, rejected);
                    notify(callbacks, rejected);
                } catch (...) {
                    // Keep processing all detached workers. A true OOM can
                    // still prevent notifying an awaiter; it must never
                    // prevent the group from requesting actor shutdown.
                }
            }
            if (worker.phase != Phase::stopping) {
                try { worker.stop_request(); }
                catch (...) {
                    // Other workers' stop requests still run.
                }
            }
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
        if (it == workers_.end() || !(it->second.ref == ref)
            || it->second.ref.ticket_ != ref.ticket_) return nullptr;
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
