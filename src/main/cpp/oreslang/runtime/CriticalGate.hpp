#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
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

    CriticalGate() : scope_(next_scope_.fetch_add(1, std::memory_order_relaxed) + 1) {
        if (scope_ == 0) throw std::overflow_error("critical scope ids exhausted");
    }
    CriticalGate(const CriticalGate&) = delete;
    CriticalGate& operator=(const CriticalGate&) = delete;

    /** Never blocks a scheduler carrier; requests a unique logical ticket. */
    [[nodiscard]] Request request() {
        std::lock_guard<std::mutex> lock(mu_);
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
        return token && token.scope_ == scope_ && owner_ == token;
    }

    [[nodiscard]] std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

private:
    void validate(Token token) const {
        if (!token || token.scope_ != scope_ || token.sequence_ > issued_) {
            throw std::invalid_argument("foreign or invalid critical ticket");
        }
    }

    inline static std::atomic<std::uint64_t> next_scope_{0};
    const std::uint64_t scope_;
    mutable std::mutex mu_;
    std::uint64_t issued_ = 0;
    Token owner_;
    std::deque<Token> waiting_;
};

} // namespace oreslang::runtime
