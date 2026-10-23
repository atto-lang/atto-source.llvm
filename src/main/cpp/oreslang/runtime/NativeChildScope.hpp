#pragma once
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {
/** Host-only per-parent lifetime ledger, not a supervisor or thread pool.
 * Reserve before spawn publication; abort only if the actor never escaped.
 * Join proves actor done tickets, NOT separate physical arena/FFI loan release.
 */
class NativeChildScope final {
public:
    using Stop = std::function<void()>;
    NativeChildScope(NativeActorRef parent, std::size_t capacity)
        : parent_(std::move(parent)), state_(std::make_shared<State>(capacity)) {
        if (!capacity) throw std::invalid_argument("zero child capacity");
    }
    NativeChildScope(const NativeChildScope&) = delete;
    NativeChildScope& operator=(const NativeChildScope&) = delete;
    ~NativeChildScope() noexcept { try { close(); } catch (...) {} }

    std::uint64_t reserve() {
        auto s = state_;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->sealed) throw std::logic_error("child scope sealed");
        if (s->entries.size() >= s->capacity) throw std::logic_error("child scope full");
        if (s->next == UINT64_MAX) throw std::overflow_error("child token exhausted");
        const auto token = s->next++;
        s->entries.emplace(token, Entry{});
        return token;
    }

    void abort(std::uint64_t token) {
        auto s = state_;
        bool publish;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            auto it = s->entries.find(token);
            if (it == s->entries.end() || it->second.child)
                throw std::logic_error("cannot abort published child");
            s->entries.erase(it);
            publish = joined_locked(*s);
        }
        if (publish) s->done.set_value();
    }

    void attach(std::uint64_t token, NativeActorRef child, Stop stop) {
        if (!stop) throw std::invalid_argument("missing stop callback");
        if (child.runtime_id() != parent_.runtime_id() ||
            (child.actor_id() == parent_.actor_id() &&
             child.generation() == parent_.generation()))
            throw std::invalid_argument("foreign/self child");
        auto s = state_;
        bool stop_now = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            auto it = s->entries.find(token);
            if (it == s->entries.end() || it->second.child)
                throw std::logic_error("duplicate/stale child token");
            for (const auto& pair : s->entries)
                if (pair.second.child && *pair.second.child == child)
                    throw std::logic_error("duplicate actor incarnation");
            it->second.child = child;
            it->second.stop = stop;
            if (s->sealed) {
                it->second.stop_issued = true;
                stop_now = true;
            }
        }
        // Register before invoking an asynchronously or synchronously
        // completing stop. Never call guest code in this finalizer callback.
        child.when_done([s, token, child](const ActorDoneResult& result) {
            bool publish;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                auto it = s->entries.find(token);
                if (it == s->entries.end() || !it->second.child ||
                    !(*it->second.child == child)) return;
                if (!result.success && s->failed != UINT64_MAX) ++s->failed;
                s->entries.erase(it);
                publish = joined_locked(*s);
            }
            if (publish) s->done.set_value();
        });
        if (stop_now) issue_stop(s, stop);
    }

    /** Seal admission and issue stop once; do not park a carrier for join. */
    void close() {
        auto s = state_;
        std::vector<Stop> callbacks;
        bool publish;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->sealed = true;
            for (auto& pair : s->entries) {
                auto& entry = pair.second;
                if (!entry.child || entry.stop_issued) continue;
                entry.stop_issued = true;
                callbacks.push_back(entry.stop);
            }
            publish = joined_locked(*s);
        }
        for (const auto& cb : callbacks) issue_stop(s, cb);
        if (publish) s->done.set_value();
    }

    std::shared_future<void> get_done() const { return state_->future; }
    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->entries.size();
    }
    std::uint64_t failures() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->failed;
    }
    std::uint64_t stop_failures() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->stop_failures;
    }

private:
    struct Entry {
        std::optional<NativeActorRef> child;
        Stop stop;
        bool stop_issued = false;
    };
    struct State {
        explicit State(std::size_t cap)
            : capacity(cap), future(done.get_future().share()) {}
        std::mutex mutex;
        const std::size_t capacity;
        std::uint64_t next = 1;
        std::map<std::uint64_t, Entry> entries;
        bool sealed = false, published = false;
        std::uint64_t failed = 0, stop_failures = 0;
        std::promise<void> done;
        std::shared_future<void> future;
    };
    static bool joined_locked(State& s) {
        if (!s.sealed || !s.entries.empty() || s.published) return false;
        s.published = true;
        return true;
    }
    static void issue_stop(const std::shared_ptr<State>& s, const Stop& cb) {
        try { cb(); } catch (...) {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->stop_failures != UINT64_MAX) ++s->stop_failures;
            // A failed stop request does NOT certify child finalization.
        }
    }
    NativeActorRef parent_;
    std::shared_ptr<State> state_;
};
} // namespace oreslang::runtime
