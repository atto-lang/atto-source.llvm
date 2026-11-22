#pragma once

#include "oreslang/runtime/CriticalGate.hpp"

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Actor-scheduler logical mutex: a capacity-one work admission queue with
 * one live execution token, not a thread-affine native mutex.
 *
 * The scheduler requests a ticket, suspends any queued invocation, and enqueues
 * the successor returned from release() on its control/resume lane. No worker
 * thread is parked waiting for the protected payload.
 *
 * This native *substrate* intentionally does NOT publish source Mutex<T> or
 * SharedMutex<T> semantics. An LLVM ownership checker and scheduler must prove
 * that the current invocation owns token while accessing state, that borrows
 * cannot escape a turn, and that release waits for the last active access.
 * Until that lowering exists this wrapper is only valid under such a contract.
 */
template <typename T>
class LogicalMutex final {
public:
    using Token = CriticalGate::Token;
    using Admission = CriticalGate::Admission;
    using Request = CriticalGate::Request;

    template <typename... Args>
    explicit LogicalMutex(Args&&... args)
        : state_(std::forward<Args>(args)...) {}

    LogicalMutex(const LogicalMutex&) = delete;
    LogicalMutex& operator=(const LogicalMutex&) = delete;

    [[nodiscard]] Request request() { return queue_.request(); }
    [[nodiscard]] bool owns(Token token) const { return queue_.holds(token); }
    [[nodiscard]] std::size_t waiting() const { return queue_.waiting(); }

    /**
     * Scheduler-only handoff. If an owner is released, return the next eligible
     * ticket, which the scheduler must resume separately (never inline).
     */
    [[nodiscard]] Token release(Token token) {
        std::lock_guard<std::mutex> lock(lease_mu_);
        if (active_ && queue_.holds(token)) {
            throw std::logic_error("cannot release logical mutex during protected callback");
        }
        return queue_.release(token);
    }

    /**
     * Only an *already-admitted* logical invocation may inspect or mutate T.
     * There is intentionally no OS lock around user code. The compiler/scheduler
     * must ensure release cannot race an executing callback of this token.
     * Raw pointers/references may not be returned from the callback.
     */
    template <typename Fn>
    auto with_owner(Token token, Fn&& fn)
            -> std::invoke_result_t<Fn&, T&> {
        using Result = std::invoke_result_t<Fn&, T&>;
        static_assert(!std::is_reference_v<Result> && !std::is_pointer_v<Result>,
                      "native logical mutex protected references cannot escape");
        {
            std::lock_guard<std::mutex> lock(lease_mu_);
            if (!queue_.holds(token)) {
                throw std::logic_error("native logical mutex access without owning ticket");
            }
            if (active_) {
                throw std::logic_error("simultaneous logical mutex protected callbacks");
            }
            active_ = true;
        }
        struct LeaseEnd final {
            LogicalMutex& mutex;
            ~LeaseEnd() noexcept {
                std::lock_guard<std::mutex> lock(mutex.lease_mu_);
                mutex.active_ = false;
            }
        } leaseEnd{*this};
        if constexpr (std::is_void_v<Result>) {
            std::forward<Fn>(fn)(state_);
        } else {
            return std::forward<Fn>(fn)(state_);
        }
    }

private:
    CriticalGate queue_;
    mutable std::mutex lease_mu_;
    bool active_ = false;
    T state_;
};

} // namespace oreslang::runtime
