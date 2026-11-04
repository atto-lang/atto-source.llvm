#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace oreslang::runtime {

/**
 * A capacity-one, FIFO logical invocation queue; never an OS mutex held
 * during guest execution. Only short queue admission and handoff operations
 * acquire mu_. The owning actor/scheduler awaits separately.
 *
 * request() issues gate-scoped, non-reusable tokens, so stale completions
 * and tickets from other scopes cannot release a new invocation. release()
 * returns the newly admitted token for the scheduler to enqueue; it MUST NOT
 * execute guest continuations inside release() or while holding mu_.
 */
class CriticalGate final {
public:
    class Token final {
        friend class CriticalGate;
    public:
        Token() = default;
        explicit operator bool() const noexcept { return sequence_ != 0; }
        bool operator==(const Token& other) const noexcept {
            return scope_ == other.scope_ && sequence_ == other.sequence_;
        }
        bool operator!=(const Token& other) const noexcept { return !(*this == other); }
    private:
        Token(std::uint64_t scope, std::uint64_t sequence)
            : scope_(scope), sequence_(sequence) {}
        std::uint64_t scope_ = 0;
        std::uint64_t sequence_ = 0;
    };

    enum class Admission { acquired, queued };
    struct Request final {
        Token token;
        Admission admission;
    };
    static constexpr std::size_t max_waiters = 8192;

    /** Exact FIFO cancellation receipts for the authenticated scheduler.
     * Never call scheduler/guest callbacks while holding the metadata lock.
     * The owner may still be executing physically; retirement alone cannot
     * interrupt or roll back an in-flight critical body.
     */
    struct Retirement final {
        std::deque<Token> cancelled_waiters;
        Token revoked_owner;
    };

    CriticalGate() : scope_(issue_scope()) {}
    CriticalGate(const CriticalGate&) = delete;
    CriticalGate& operator=(const CriticalGate&) = delete;

    /**
     * Immediate acquisition for source "try critical" lowering. Never enqueues
     * and does not wait for the internal short bookkeeping mutex, either.
     * Failure is not a ticket and cannot later acquire a guard.
     */
    [[nodiscard]] std::optional<Token> try_request() {
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock || closed_ || owner_ || !waiting_.empty())
            return std::nullopt;
        if (issued_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("critical ticket sequence exhausted");
        owner_ = Token(scope_, ++issued_);
        return owner_;
    }

    /** Briefly synchronizes ticket admission; never holds an OS mutex in guest code. */
    [[nodiscard]] Request request() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) throw std::logic_error("critical gate retired");
        const bool available = !owner_ && waiting_.empty();
        if (!available && waiting_.size() >= max_waiters) {
            throw std::overflow_error("critical waiter limit exceeded");
        }
        if (issued_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("critical ticket sequence exhausted");
        }
        Token token(scope_, ++issued_);
        if (available) {
            owner_ = token;
            return {token, Admission::acquired};
        }
        waiting_.push_back(token);
        return {token, Admission::queued};
    }

    /**
     * Release an owner or cancel a waiter. Return the next permitted ticket,
     * or an empty Token. Scheduler must enqueue only live admitted tickets,
     * and recheck holds(token) before executing the resumed guest continuation.
     */
    [[nodiscard]] Token release(Token token) {
        std::lock_guard<std::mutex> lock(mu_);
        validate(token);
        if (closed_) return {};
        if (owner_ != token) {
            for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
                if (*it == token) {
                    waiting_.erase(it);
                    return {};
                }
            }
            return {}; // idempotent stale/duplicate release
        }
        owner_ = {};
        if (waiting_.empty()) return {};
        owner_ = waiting_.front();
        waiting_.pop_front();
        return owner_;
    }

    [[nodiscard]] bool holds(Token token) const {
        std::lock_guard<std::mutex> lock(mu_);
        return !closed_ && token && token.scope_ == scope_ && owner_ == token;
    }

    [[nodiscard]] std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

    /**
     * Revoke owner and queued tickets at actor-generation retirement. Caller
     * MUST first quiesce the physically running body: this fences future
     * admission but cannot interrupt a guest critical mutation mid-instruction.
     * No callbacks run under the bookkeeping mutex.
     */
    [[nodiscard]] Retirement retire() {
        std::lock_guard<std::mutex> lock(mu_);
        Retirement receipt;
        if (closed_) return receipt; // exactly-once generation fence
        closed_ = true;
        receipt.revoked_owner = owner_;
        owner_ = {};
        receipt.cancelled_waiters.swap(waiting_);
        return receipt;
    }

    /** Logical ticket quiescence only. A scheduler must independently prove
     * that all physically executing guest critical bodies have exited.
     */
    [[nodiscard]] bool quiescent() const {
        std::lock_guard<std::mutex> lock(mu_);
        return !owner_ && waiting_.empty();
    }

    /** Atomic empty-gate retirement: a concurrent request either wins the
     * ticket or sees the closed gate. Never split observation from closure.
     * Returns false if the current generation owns or queues any ticket.
     */
    [[nodiscard]] bool retire_if_quiescent() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return true;
        if (owner_ || !waiting_.empty()) return false;
        closed_ = true;
        return true;
    }

    [[nodiscard]] bool retired() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

private:
    [[nodiscard]] static std::uint64_t issue_scope() {
        auto current = next_scope_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("critical scope ids exhausted");
            if (next_scope_.compare_exchange_weak(current, current + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return current + 1;
        }
    }

    void validate(Token token) const {
        if (!token || token.scope_ != scope_ || token.sequence_ > issued_) {
            throw std::invalid_argument("foreign or invalid critical ticket");
        }
    }

    inline static std::atomic<std::uint64_t> next_scope_{0};
    const std::uint64_t scope_;
    mutable std::mutex mu_;
    std::uint64_t issued_ = 0;
    bool closed_ = false;
    Token owner_;
    std::deque<Token> waiting_;
};

} // namespace oreslang::runtime
