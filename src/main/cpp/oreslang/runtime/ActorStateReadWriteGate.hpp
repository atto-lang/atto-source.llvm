#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace oreslang::runtime {

/**
 * FIFO, continuation-owned logical read/write admission. It does NOT permit
 * concurrent execution of the same actor's guest code. Shared read tickets
 * are suitable only for verified immutable snapshots/read tasks outside the
 * single-flight actor turn; state-backed mutable loans remain actor-exclusive.
 *
 * The short mutex protects only queue metadata. Never execute a guest
 * continuation while holding it, and never park a scheduler carrier on it.
 */
class ActorStateReadWriteGate final {
public:
    enum class Mode : std::uint8_t { Read, Write };
    enum class Admission : std::uint8_t { acquired, queued };

    class Token final {
        friend class ActorStateReadWriteGate;
    public:
        Token() = default;
        explicit operator bool() const noexcept { return sequence_ != 0; }
        bool operator==(const Token& t) const noexcept {
            return scope_ == t.scope_ && sequence_ == t.sequence_;
        }
    private:
        Token(std::uint64_t scope, std::uint64_t sequence)
            : scope_(scope), sequence_(sequence) {}
        std::uint64_t scope_ = 0;
        std::uint64_t sequence_ = 0;
    };

    struct Request final {
        Token token;
        Admission admission;
    };

    static constexpr std::size_t max_waiters = 8192;

    ActorStateReadWriteGate() : scope_(mint_scope()) {}
    ActorStateReadWriteGate(const ActorStateReadWriteGate&) = delete;
    ActorStateReadWriteGate& operator=(const ActorStateReadWriteGate&) = delete;

    [[nodiscard]] Request request(Mode mode) {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) throw std::logic_error("actor state gate retired");
        // Once a writer has queued, later readers cannot bypass it.
        const bool available = waiting_.empty() &&
                (mode == Mode::Read ? !writer_active_ : owners_.empty());
        if (!available && waiting_.size() >= max_waiters) {
            throw std::overflow_error("actor state gate waiter limit exceeded");
        }
        if (issued_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("actor state ticket sequence exhausted");
        }
        Token token(scope_, ++issued_);
        if (available) {
            owners_.emplace(token.sequence_, mode);
            if (mode == Mode::Write) writer_active_ = true;
            return {token, Admission::acquired};
        }
        waiting_.push_back({token, mode});
        return {token, Admission::queued};
    }

    /**
     * Release an admitted reader/writer or cancel a pending ticket.
     * Returns all newly admitted tickets for scheduler CONTROL resumption.
     * Recheck holds(token, mode) before each resumed operation.
     */
    [[nodiscard]] std::vector<Token> release(Token token) {
        std::lock_guard<std::mutex> lock(mu_);
        validate(token);
        if (closed_) return {};
        auto owner = owners_.find(token.sequence_);
        if (owner != owners_.end()) {
            if (owner->second == Mode::Write) writer_active_ = false;
            owners_.erase(owner);
        } else {
            for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
                if (it->token == token) {
                    waiting_.erase(it);
                    return admit_waiters();
                }
            }
            return {}; // stale or duplicate completion
        }
        return admit_waiters();
    }

    [[nodiscard]] bool holds(Token token, Mode mode) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || !token || token.scope_ != scope_) return false;
        auto it = owners_.find(token.sequence_);
        return it != owners_.end() && it->second == mode;
    }

    [[nodiscard]] std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

    [[nodiscard]] std::size_t active_readers() const {
        std::lock_guard<std::mutex> lock(mu_);
        return writer_active_ ? 0 : owners_.size();
    }

    // Retirement invalidates all outstanding capabilities.
    void retire() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        owners_.clear();
        waiting_.clear();
        writer_active_ = false;
    }

private:
    struct Waiter { Token token; Mode mode; };

    static std::uint64_t mint_scope() {
        auto n = next_scope_.load(std::memory_order_relaxed);
        for (;;) {
            if (n == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("actor state gate scopes exhausted");
            }
            if (next_scope_.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {
                return n + 1;
            }
        }
    }

    void validate(Token token) const {
        if (!token || token.scope_ != scope_ || token.sequence_ > issued_) {
            throw std::invalid_argument("foreign or invalid actor state ticket");
        }
    }

    [[nodiscard]] std::vector<Token> admit_waiters() {
        std::vector<Token> admitted;
        if (closed_ || writer_active_ || waiting_.empty()) return admitted;
        if (waiting_.front().mode == Mode::Write) {
            if (owners_.empty()) {
                const auto w = waiting_.front();
                waiting_.pop_front();
                owners_.emplace(w.token.sequence_, Mode::Write);
                writer_active_ = true;
                admitted.push_back(w.token);
            }
            return admitted;
        }
        // Admit the maximal consecutive reader cohort before the next writer.
        // Existing readers may still be active when an earlier writer cancels.
        while (!waiting_.empty() && waiting_.front().mode == Mode::Read) {
            const auto w = waiting_.front();
            waiting_.pop_front();
            owners_.emplace(w.token.sequence_, Mode::Read);
            admitted.push_back(w.token);
        }
        return admitted;
    }

    inline static std::atomic<std::uint64_t> next_scope_{0};
    const std::uint64_t scope_;
    mutable std::mutex mu_;
    std::uint64_t issued_ = 0;
    bool writer_active_ = false;
    bool closed_ = false;
    std::unordered_map<std::uint64_t, Mode> owners_;
    std::deque<Waiter> waiting_;
};

} // namespace oreslang::runtime
