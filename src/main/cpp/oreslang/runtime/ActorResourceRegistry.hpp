#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Trusted native runtime substrate.  No raw resource or callback may be sent
// across a guest actor boundary.  A registry is owned by one actor incarnation.
class ActorResourceRegistry final {
public:
    enum class Kind { socket, file, timer, ffi_callback, native_allocation };
    enum class Phase { live, retiring, closing, reclaimed };
    enum class Reclaim { not_retired, busy, closing, complete };

    struct Result {
        Reclaim status;
        std::size_t closed;
        std::vector<std::string> errors;
    };

    explicit ActorResourceRegistry(std::uint64_t actor_id,
                                   std::uint64_t incarnation,
                                   std::size_t max_resources = 256)
        : actor_id_(actor_id), incarnation_(incarnation), capacity_(max_resources) {
        if (!actor_id || !incarnation || max_resources == 0 || max_resources > 4096)
            throw std::invalid_argument("invalid actor resource registry identity/capacity");
        entries_.reserve(max_resources);
    }

    ActorResourceRegistry(const ActorResourceRegistry&) = delete;
    ActorResourceRegistry& operator=(const ActorResourceRegistry&) = delete;

    std::uint64_t actor_id() const noexcept { return actor_id_; }
    std::uint64_t incarnation() const noexcept { return incarnation_; }

    // Host-only registration. Callbacks must be trusted, idempotent close or
    // revocation actions. Guest functions and blocking I/O are not admitted.
    void register_resource(Kind kind, std::function<void()> close) {
        if (!close) throw std::invalid_argument("resource closer is required");
        std::lock_guard<std::mutex> guard(mutex_);
        require_live();
        if (entries_.size() >= capacity_)
            throw std::length_error("actor resource registry capacity exceeded");
        entries_.push_back({kind, std::move(close)});
    }

    // Scheduler-only tokens. Tokens are observations of outstanding activity,
    // never authority to dereference actor memory on foreign threads.
    std::uint64_t acquire_turn() { return acquire(turns_); }
    std::uint64_t acquire_loan() { return acquire(loans_); }
    std::uint64_t acquire_callback() { return acquire(callbacks_); }
    void release_turn(std::uint64_t token) { release(turns_, token); }
    void release_loan(std::uint64_t token) { release(loans_, token); }
    void release_callback(std::uint64_t token) { release(callbacks_, token); }

    void retire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ == Phase::live) phase_ = Phase::retiring;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return phase_;
    }

    // Physical reclamation must only follow scheduler/FFI quiescence.
    // Never invoke a resource closer with the registry mutex held.
    Result try_reclaim() {
        std::vector<Entry> pending;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (phase_ == Phase::live) return {Reclaim::not_retired, 0, {}};
            if (phase_ == Phase::closing) return {Reclaim::closing, 0, {}};
            if (phase_ == Phase::reclaimed) return last_result_;
            if (!turns_.empty() || !loans_.empty() || !callbacks_.empty())
                return {Reclaim::busy, 0, {}};
            phase_ = Phase::closing;
            pending.swap(entries_);
        }
        Result result{Reclaim::complete, 0, {}};
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            try {
                it->close();
            } catch (const std::exception& e) {
                if (result.errors.size() < 16) result.errors.emplace_back(e.what());
            } catch (...) {
                if (result.errors.size() < 16) result.errors.emplace_back("unknown closer failure");
            }
            ++result.closed;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            last_result_ = result;
            phase_ = Phase::reclaimed;
        }
        return result;
    }

private:
    struct Entry {
        Kind kind;
        std::function<void()> close;
    };

    void require_live() const {
        if (phase_ != Phase::live) throw std::logic_error("actor resource registry retired");
    }

    std::uint64_t acquire(std::unordered_set<std::uint64_t>& set) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_live();
        if (next_token_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor resource token exhausted");
        const auto token = ++next_token_;
        set.insert(token);
        return token;
    }

    void release(std::unordered_set<std::uint64_t>& set, std::uint64_t token) {
        std::lock_guard<std::mutex> guard(mutex_);
        // Releases remain admissible during retirement; duplicates and unknown
        // tokens are errors. No new loans, callbacks, or turns may be acquired.
        if (set.erase(token) != 1)
            throw std::logic_error("invalid or duplicate actor resource lease release");
    }

    const std::uint64_t actor_id_;
    const std::uint64_t incarnation_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    Phase phase_ = Phase::live;
    std::uint64_t next_token_ = 0;
    std::vector<Entry> entries_;
    std::unordered_set<std::uint64_t> turns_, loans_, callbacks_;
    Result last_result_{Reclaim::not_retired, 0, {}};
};

} // namespace oreslang::runtime
