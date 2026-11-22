#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
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
    static constexpr std::size_t max_active_readers = 1024;

    ActorStateReadWriteGate() : scope_(mint_scope()) {}
    ActorStateReadWriteGate(const ActorStateReadWriteGate&) = delete;
    ActorStateReadWriteGate& operator=(const ActorStateReadWriteGate&) = delete;

    [[nodiscard]] Request request(Mode mode) {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) throw std::logic_error("actor state gate retired");
        if (mode != Mode::Read && mode != Mode::Write) {
            throw std::invalid_argument("unsupported reader/writer mode");
        }
        // Once a writer has queued, later readers cannot bypass it.
        const bool available = waiting_.empty() &&
                (mode == Mode::Read ? !writer_active_ && owners_.size() < max_active_readers
                                    : owners_.empty());
        if (!available && waiting_.size() >= max_waiters) {
            throw std::overflow_error("actor state gate waiter limit exceeded");
        }
        if (issued_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("actor state ticket sequence exhausted");
        }
        Token token(scope_, ++issued_);
        if (available) {
            owners_.push_back({token, mode});
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
        const auto owner = std::find_if(owners_.begin(), owners_.end(),
                [&](const Waiter& w) { return w.token == token; });
        if (owner == owners_.end()) {
            const auto waiter = std::find_if(waiting_.begin(), waiting_.end(),
                    [&](const Waiter& w) { return w.token == token; });
            if (waiter == waiting_.end()) return {}; // stale/duplicate completion
            // Allocate wake capacity BEFORE changing queue state. Splicing
            // existing list nodes into active owners is thereafter noexcept.
            std::vector<Token> admitted;
            admitted.reserve(waiting_.size());
            waiting_.erase(waiter);
            admit_waiters(admitted);
            return admitted;
        }
        std::vector<Token> admitted;
        admitted.reserve(waiting_.size());
        const bool was_writer = owner->mode == Mode::Write;
        owners_.erase(owner);
        if (was_writer) writer_active_ = false;
        admit_waiters(admitted);
        return admitted;
    }

    [[nodiscard]] bool holds(Token token, Mode mode) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || !token || token.scope_ != scope_) return false;
        return std::any_of(owners_.begin(), owners_.end(), [&](const Waiter& w) {
            return w.token == token && w.mode == mode;
        });
    }

    [[nodiscard]] std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

    [[nodiscard]] std::size_t active_readers() const {
        std::lock_guard<std::mutex> lock(mu_);
        return writer_active_ ? 0 : owners_.size();
    }

    /**
     * Atomic retirement returns active and queued tokens to the trusted
     * scheduler so every suspended waiter can receive terminal cancellation.
     * If allocation fails, no authority is revoked or silently discarded.
     * The scheduler must publish cancellations outside this metadata lock.
     */
    [[nodiscard]] std::vector<Token> retire() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return {};
        std::vector<Token> revoked;
        revoked.reserve(owners_.size() + waiting_.size());
        for (const auto& w : owners_) revoked.push_back(w.token);
        for (const auto& w : waiting_) revoked.push_back(w.token);
        closed_ = true;
        owners_.clear();
        waiting_.clear();
        writer_active_ = false;
        return revoked;
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

    // Precondition: caller has reserved admitted.capacity >= waiting_.size()
    // before changing active/waiting ownership. No allocation on handoff.
    void admit_waiters(std::vector<Token>& admitted) {
        if (closed_ || writer_active_ || waiting_.empty()) return;
        if (waiting_.front().mode == Mode::Write) {
            if (owners_.empty()) {
                const Token ticket = waiting_.front().token;
                owners_.splice(owners_.end(), waiting_, waiting_.begin());
                writer_active_ = true;
                admitted.push_back(ticket);
            }
            return;
        }
        while (!waiting_.empty() && waiting_.front().mode == Mode::Read
                && owners_.size() < max_active_readers) {
            const Token ticket = waiting_.front().token;
            owners_.splice(owners_.end(), waiting_, waiting_.begin());
            admitted.push_back(ticket);
        }
    }

    inline static std::atomic<std::uint64_t> next_scope_{0};
    const std::uint64_t scope_;
    mutable std::mutex mu_;
    std::uint64_t issued_ = 0;
    bool writer_active_ = false;
    bool closed_ = false;
    std::list<Waiter> owners_;
    std::list<Waiter> waiting_;
};

} // namespace oreslang::runtime
