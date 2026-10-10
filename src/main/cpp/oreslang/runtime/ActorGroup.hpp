#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Host-side lifecycle substrate for native actors. This does NOT yet compile
// source ActorGroup, spawn, await, or actor declarations. The native scheduler
// must notify readiness after on_start, and finalization after all turn/loan/
// waker roots are gone; no carrier is ever blocked on these observer futures.
namespace oreslang::runtime {

class ActorGroup final {
    enum class Phase { starting, ready, stopping, terminal };

    struct WorkerState {
        const std::uint64_t group;
        const std::uint64_t id;
        std::function<void()> request_stop;
        std::promise<void> ready_promise;
        std::promise<void> done_promise;
        std::shared_future<void> ready_future;
        std::shared_future<void> done_future;
        Phase phase = Phase::starting; // guarded by ActorGroup::mutex_
        bool stop_requested = false;  // guarded by ActorGroup::mutex_

        WorkerState(std::uint64_t group_id, std::uint64_t worker_id,
                    std::function<void()> stop)
            : group(group_id), id(worker_id), request_stop(std::move(stop)),
              ready_future(ready_promise.get_future().share()),
              done_future(done_promise.get_future().share()) {}
    };

public:
    // An observer token; NOT a native lifecycle/cancel capability. Group IDs
    // and monotonically allocated worker IDs fence stale or foreign refs.
    class ActorRef final {
    public:
        std::uint64_t id() const noexcept { return state_ ? state_->id : 0; }
        std::uint64_t group_id() const noexcept {
            return state_ ? state_->group : 0;
        }
        std::shared_future<void> ready() const {
            require();
            return state_->ready_future;
        }
        std::shared_future<void> done() const {
            require();
            return state_->done_future;
        }
        explicit operator bool() const noexcept { return bool(state_); }

    private:
        friend class ActorGroup;
        explicit ActorRef(std::shared_ptr<WorkerState> state)
            : state_(std::move(state)) {}
        void require() const {
            if (!state_) throw std::logic_error("invalid native ActorRef");
        }
        std::shared_ptr<WorkerState> state_;
    };

    explicit ActorGroup(std::size_t max_workers)
        : group_id_(next_group_id_.fetch_add(1, std::memory_order_relaxed)),
          max_workers_(max_workers) {
        if (max_workers == 0) throw std::invalid_argument("max_workers must be positive");
        if (group_id_ == 0) throw std::overflow_error("native ActorGroup identity exhausted");
    }

    ~ActorGroup() noexcept {
        try { close(); } catch (...) { /* destructor cannot wait or throw */ }
    }

    ActorGroup(const ActorGroup&) = delete;
    ActorGroup& operator=(const ActorGroup&) = delete;

    std::uint64_t id() const noexcept { return group_id_; }

    // Called by the supervisor BEFORE worker startup can race completion.
    // Returns immediately with an unready ref (the equivalent of 'spawn').
    ActorRef register_spawn(std::function<void()> stop) {
        if (!stop) throw std::invalid_argument("worker stop hook required");
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) throw std::logic_error("ActorGroup is closed");
        if (workers_.size() >= max_workers_) throw std::length_error("ActorGroup worker quota");
        if (next_worker_id_ == 0) throw std::overflow_error("worker identity exhausted");
        auto state = std::make_shared<WorkerState>(group_id_, next_worker_id_++, std::move(stop));
        workers_.emplace(state->id, state);
        return ActorRef{std::move(state)};
    }

    // Called by the runtime ONLY after on_start and required field checks.
    // Future waiters are observers; this method is never executed by a user
    // closure on a producer/completion thread.
    bool mark_ready(const ActorRef& ref) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = find_locked(ref);
        if (!state || state->phase != Phase::starting || closed_) return false;
        state->phase = Phase::ready;
        state->ready_promise.set_value();
        return true;
    }

    // Startup failure terminates this managed entry. The native spawn path
    // must finish physical cleanup before calling this finalization boundary.
    bool startup_failed(const ActorRef& ref, std::exception_ptr error) {
        if (!error) throw std::invalid_argument("startup failure cause required");
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = find_locked(ref);
        if (!state || state->phase != Phase::starting) return false;
        state->phase = Phase::terminal;
        state->ready_promise.set_exception(error);
        state->done_promise.set_exception(error);
        workers_.erase(state->id);
        return true;
    }

    // Signal the FINAL native actor exit after active turns, suspended
    // continuations, wake registrations and native memory loans are quiescent.
    bool finalized(const ActorRef& ref, std::exception_ptr failure = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = find_locked(ref);
        if (!state || state->phase == Phase::terminal) return false;
        if (state->phase == Phase::starting) {
            state->ready_promise.set_exception(std::make_exception_ptr(
                std::runtime_error("actor finalized before readiness")));
        }
        state->phase = Phase::terminal;
        if (failure) state->done_promise.set_exception(failure);
        else state->done_promise.set_value();
        workers_.erase(state->id);
        return true;
    }

    // Manager-only API: external supervision must control access to this
    // method. The hook requests cooperative structured cancellation; it
    // cannot synchronously terminate uncooperative native/FFI code.
    bool request_stop(const ActorRef& ref) {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto state = find_locked(ref);
            if (!state || state->stop_requested) return false;
            state->stop_requested = true;
            if (state->phase == Phase::starting) {
                state->ready_promise.set_exception(std::make_exception_ptr(
                    std::runtime_error("actor stopped before readiness")));
            }
            state->phase = Phase::stopping;
            hook = state->request_stop;
        }
        hook(); // NEVER invoke guest/control callbacks under group lock
        return true;
    }

    std::vector<ActorRef> workers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ActorRef> refs;
        refs.reserve(workers_.size());
        for (const auto& item : workers_) refs.push_back(ActorRef{item.second});
        return refs; // point-in-time snapshot
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return workers_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // Seal registration before requesting shutdown; do not manufacture a
    // 'done' result. Runtime must deliver one finalization ack per worker.
    void close() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
            callbacks.reserve(workers_.size());
            for (auto& item : workers_) {
                auto& state = item.second;
                if (state->stop_requested) continue;
                state->stop_requested = true;
                if (state->phase == Phase::starting) {
                    state->ready_promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("ActorGroup closed before actor readiness")));
                }
                state->phase = Phase::stopping;
                callbacks.push_back(state->request_stop);
            }
        }
        for (auto& callback : callbacks) {
            try { callback(); } catch (...) { /* finalization ack still required */ }
        }
    }

private:
    std::shared_ptr<WorkerState> find_locked(const ActorRef& ref) const {
        if (!ref.state_ || ref.state_->group != group_id_) return {};
        auto it = workers_.find(ref.state_->id);
        if (it == workers_.end() || it->second != ref.state_) return {};
        return it->second;
    }

    inline static std::atomic<std::uint64_t> next_group_id_{1};
    const std::uint64_t group_id_;
    const std::size_t max_workers_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<WorkerState>> workers_;
    std::uint64_t next_worker_id_ = 1;
    bool closed_ = false;
};
} // namespace oreslang::runtime
