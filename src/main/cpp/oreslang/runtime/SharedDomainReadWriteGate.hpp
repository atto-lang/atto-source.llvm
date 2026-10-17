#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

/*
 * Bounded FIFO logical admission for one shared-domain arena.
 *
 * This gate never runs guest code and never parks an actor carrier. A short
 * host mutex protects queue metadata only. Once a writer queues, later readers
 * cannot bypass it; adjacent readers at the front form a bounded cohort.
 *
 * Physical byte authority still comes from SharedDomainArena::Lease. A ticket
 * alone is scheduler metadata and cannot read or write memory.
 */
class SharedDomainReadWriteGate final {
public:
    enum class Mode : std::uint8_t { Read, Write };
    enum class Admission : std::uint8_t { Acquired, Queued };

    class Token final {
        friend class SharedDomainReadWriteGate;
    public:
        Token() = default;
        explicit operator bool() const noexcept { return sequence_ != 0; }
        bool operator==(const Token& rhs) const noexcept {
            return scope_ == rhs.scope_ && sequence_ == rhs.sequence_;
        }
        std::uint64_t scope() const noexcept { return scope_; }
        std::uint64_t sequence() const noexcept { return sequence_; }
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
    struct Ready final {
        Token token;
        OresNativeActorIdentity owner;
        Mode mode;
    };

    static constexpr std::size_t max_waiters = 8192;
    static constexpr std::size_t max_active_readers = 1024;

    SharedDomainReadWriteGate() : scope_(mint_scope()) {}
    SharedDomainReadWriteGate(const SharedDomainReadWriteGate&) = delete;
    SharedDomainReadWriteGate& operator=(const SharedDomainReadWriteGate&) = delete;

    [[nodiscard]] Request request(OresNativeActorIdentity owner, Mode mode) {
        require_identity(owner);
        if (mode != Mode::Read && mode != Mode::Write)
            throw std::invalid_argument("unsupported shared-domain access mode");
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) throw std::logic_error("shared-domain gate retired");
        const bool available = waiting_.empty()
            && (mode == Mode::Read
                ? !writer_active_ && owners_.size() < max_active_readers
                : owners_.empty());
        if (!available && waiting_.size() >= max_waiters)
            throw std::overflow_error("shared-domain waiter limit exceeded");
        if (issued_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("shared-domain ticket sequence exhausted");

        const Token token(scope_, ++issued_);
        if (available) {
            owners_.push_back({token, owner, mode});
            if (mode == Mode::Write) writer_active_ = true;
            return {token, Admission::Acquired};
        }
        waiting_.push_back({token, owner, mode});
        return {token, Admission::Queued};
    }

    [[nodiscard]] bool holds(Token token, OresNativeActorIdentity owner, Mode mode) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || !valid_identity(owner) || !token || token.scope_ != scope_)
            return false;
        return std::any_of(owners_.begin(), owners_.end(), [&](const Waiter& waiter) {
            return waiter.token == token && same_identity(waiter.owner, owner)
                && waiter.mode == mode;
        });
    }

    /*
     * Complete an admitted access or cancel an undispatched waiter. The owner
     * identity is mandatory so one actor cannot release another actor's ticket.
     * Newly-ready tickets are returned to a trusted scheduler bridge; no guest
     * callback occurs while the metadata mutex is held.
     */
    [[nodiscard]] std::vector<Ready> release(
            Token token, OresNativeActorIdentity owner) {
        require_identity(owner);
        std::lock_guard<std::mutex> lock(mu_);
        validate_token(token);
        if (closed_) return {};

        auto active = std::find_if(owners_.begin(), owners_.end(), [&](const Waiter& waiter) {
            return waiter.token == token && same_identity(waiter.owner, owner);
        });
        if (active != owners_.end()) {
            std::vector<Ready> ready;
            ready.reserve(waiting_.size());
            const bool was_writer = active->mode == Mode::Write;
            owners_.erase(active);
            if (was_writer) writer_active_ = false;
            admit_waiters(ready);
            return ready;
        }

        auto pending = std::find_if(waiting_.begin(), waiting_.end(), [&](const Waiter& waiter) {
            return waiter.token == token && same_identity(waiter.owner, owner);
        });
        if (pending == waiting_.end())
            throw std::logic_error("foreign or stale shared-domain ticket release");
        std::vector<Ready> ready;
        ready.reserve(waiting_.size());
        waiting_.erase(pending);
        admit_waiters(ready);
        return ready;
    }

    [[nodiscard]] std::vector<Ready> retire() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return {};
        std::vector<Ready> revoked;
        revoked.reserve(owners_.size() + waiting_.size());
        for (const auto& waiter : owners_)
            revoked.push_back({waiter.token, waiter.owner, waiter.mode});
        for (const auto& waiter : waiting_)
            revoked.push_back({waiter.token, waiter.owner, waiter.mode});
        closed_ = true;
        owners_.clear();
        waiting_.clear();
        writer_active_ = false;
        return revoked;
    }

    [[nodiscard]] std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

    [[nodiscard]] std::uint64_t scope() const noexcept { return scope_; }

private:
    struct Waiter {
        Token token;
        OresNativeActorIdentity owner;
        Mode mode;
    };

    static bool valid_identity(OresNativeActorIdentity identity) noexcept {
        return identity.principal != 0 && identity.incarnation != 0;
    }
    static void require_identity(OresNativeActorIdentity identity) {
        if (!valid_identity(identity))
            throw std::invalid_argument("shared-domain actor identity required");
    }
    static bool same_identity(
            OresNativeActorIdentity lhs, OresNativeActorIdentity rhs) noexcept {
        return lhs.principal == rhs.principal
            && lhs.incarnation == rhs.incarnation;
    }
    static std::uint64_t mint_scope() {
        auto current = next_scope_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("shared-domain gate scope exhausted");
            if (next_scope_.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return current + 1;
        }
    }
    void validate_token(Token token) const {
        if (!token || token.scope_ != scope_ || token.sequence_ > issued_)
            throw std::invalid_argument("foreign or invalid shared-domain ticket");
    }
    void admit_waiters(std::vector<Ready>& ready) {
        if (closed_ || writer_active_ || waiting_.empty()) return;
        if (waiting_.front().mode == Mode::Write) {
            if (owners_.empty()) {
                const Waiter admitted = waiting_.front();
                owners_.splice(owners_.end(), waiting_, waiting_.begin());
                writer_active_ = true;
                ready.push_back({admitted.token, admitted.owner, admitted.mode});
            }
            return;
        }
        while (!waiting_.empty() && waiting_.front().mode == Mode::Read
                && owners_.size() < max_active_readers) {
            const Waiter admitted = waiting_.front();
            owners_.splice(owners_.end(), waiting_, waiting_.begin());
            ready.push_back({admitted.token, admitted.owner, admitted.mode});
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
