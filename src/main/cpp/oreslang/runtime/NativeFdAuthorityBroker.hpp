#pragma once
#if !defined(__linux__)
#error "NativeFdAuthorityBroker requires Linux"
#endif
#include "oreslang/runtime/NativeFdAuthority.hpp"
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/epoll.h>

namespace oreslang::runtime {

// This broker connects the HOST-ONLY logical authority to REAL managed Linux
// eventfds and epoll tickets. It never exports a numeric FD or the manager.
// Trusted supervisors must authenticate caller identity before supplying
// owner IDs. This is NOT a guest-facing FD API or actor authorization boundary.
class NativeFdAuthorityBroker final {
public:
    static constexpr std::size_t capacity = NativeFdAuthority::capacity;
    class Token final {
        friend class NativeFdAuthorityBroker;
        std::uint64_t broker_ = 0, generation_ = 0;
        std::uint16_t slot_ = 0;
        Token(std::uint64_t broker, std::uint64_t generation, std::uint16_t slot)
            : broker_(broker), generation_(generation), slot_(slot) {}
    public:
        Token() = default;
    };
    struct Completion {
        Token token;
        NativeEpollReactor::Outcome outcome;
        std::uint32_t events;
    };

    explicit NativeFdAuthorityBroker(
        std::size_t count = 1,
        NativeEpollReactorManager::Profile profile =
            NativeEpollReactorManager::Profile::throughput,
        std::size_t rt_count = 1)
        : manager_(count, profile, rt_count), instance_(take_instance()) {}
    NativeFdAuthorityBroker(const NativeFdAuthorityBroker&) = delete;
    NativeFdAuthorityBroker& operator=(const NativeFdAuthorityBroker&) = delete;

    // Bootstrap only: eventfd is physically opened by the real manager.
    // Ledger rollback is safe on failed open because no FD was ever bound.
    std::optional<Token> mint_eventfd(std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_ || !trusted_owner ||
                next_generation_ == UINT64_MAX) return std::nullopt;
        std::size_t slot = capacity;
        for (std::size_t i = 0; i < capacity; ++i)
            if (!entries_[i].active) { slot = i; break; }
        if (slot == capacity) return std::nullopt;
        auto capability = authority_.mint(trusted_owner, NativeFdAuthority::readiness);
        if (!capability) return std::nullopt;
        auto owned = manager_.open_owned_eventfd();
        if (!owned) {
            if (!authority_.revoke(*capability, trusted_owner) ||
                    !authority_.confirm_reclaimed(*capability, trusted_owner))
                poisoned_ = true;
            return std::nullopt;
        }
        Token token{instance_, next_generation_++, static_cast<std::uint16_t>(slot)};
        entries_[slot] = Entry{true, token, trusted_owner, *capability,
                               *owned, std::nullopt};
        return token;
    }

    // Epoll admission and ledger update are serialized under broker mu_.
    // If the impossible internal ledger transition fails, quarantine rather
    // than return a watch whose terminal future cannot be authenticated.
    bool try_watch(Token token, std::uint64_t trusted_owner,
                   std::uint32_t events = EPOLLIN) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || e->watch ||
                authority_.phase(e->capability) != NativeFdAuthority::Phase::owned)
            return false;
        auto watch = manager_.try_watch(e->owned, events);
        if (!watch) return false;
        if (!authority_.begin_watch(e->capability, trusted_owner)) {
            poisoned_ = true;
            return false;
        }
        e->watch = *watch;
        return true;
    }

    bool signal(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        return e && e->watch &&
               authority_.phase(e->capability) == NativeFdAuthority::Phase::watching &&
               manager_.signal_owned_eventfd(e->owned);
    }

    // Cancellation is not a terminal receipt: try_pop must drain the result.
    bool try_cancel(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        return e && e->watch && manager_.try_cancel(*e->watch);
    }

    bool revoke(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || !authority_.revoke(e->capability, trusted_owner))
            return false;
        if (e->watch) (void)manager_.try_cancel(*e->watch);
        return true;
    }

    // No synthetic terminal: first pop the actual kernel-reactor record,
    // then publish/consume it in the authority under the broker gate.
    std::optional<Completion> try_pop() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return std::nullopt;
        for (auto& e : entries_) {
            if (e.active && e.watch &&
                    authority_.phase(e.capability) ==
                        NativeFdAuthority::Phase::revoking_watch)
                (void)manager_.try_cancel(*e.watch);
        }
        auto completed = manager_.try_pop();
        if (!completed) return std::nullopt;
        for (auto& e : entries_) {
            if (!e.active || !e.watch ||
                    e.watch->reactor != completed->reactor ||
                    e.watch->ticket.value != completed->event.ticket.value)
                continue;
            if (!authority_.publish_terminal(e.capability, e.owner) ||
                    !authority_.consume_terminal(e.capability, e.owner)) {
                poisoned_ = true;
                return std::nullopt;
            }
            e.watch.reset();
            return Completion{e.token, completed->event.outcome,
                              completed->event.events};
        }
        poisoned_ = true; // Unknown terminal must never cross ownership.
        return std::nullopt;
    }

    // Physical close succeeds only after the manager retires the epoll claim.
    // A failed close never produces logical reclamation or slot reuse.
    bool confirm_reclaimed(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || e->watch ||
                authority_.phase(e->capability) !=
                    NativeFdAuthority::Phase::reclaim_pending)
            return false;
        if (!manager_.close_owned_eventfd(e->owned)) return false;
        if (!authority_.confirm_reclaimed(e->capability, trusted_owner)) {
            poisoned_ = true; // physical close completed: quarantine
            return false;
        }
        *e = Entry{};
        return true;
    }

    // Blocking/joining teardown is trusted host-only, never an actor turn.
    void stop() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        manager_.stop();
    }
    std::size_t started_threads() const noexcept {
        return manager_.started_threads();
    }

private:
    struct Entry {
        bool active = false;
        Token token{};
        std::uint64_t owner = 0;
        NativeFdAuthority::Capability capability{};
        NativeEpollReactorManager::OwnedEventFd owned{};
        std::optional<NativeEpollReactorManager::Watch> watch{};
    };
    Entry* lookup(Token token, std::uint64_t trusted_owner) noexcept {
        if (token.broker_ != instance_ || !token.generation_ ||
                token.slot_ >= capacity) return nullptr;
        auto& e = entries_[token.slot_];
        return e.active && e.owner == trusted_owner &&
               e.token.generation_ == token.generation_ ? &e : nullptr;
    }
    static std::uint64_t take_instance() {
        auto n = next_instance_.load(std::memory_order_relaxed);
        while (n && n != UINT64_MAX) {
            if (next_instance_.compare_exchange_weak(
                    n, n + 1, std::memory_order_relaxed)) return n;
        }
        throw std::runtime_error("NIO FD broker namespaces exhausted");
    }
    inline static std::atomic<std::uint64_t> next_instance_{1};
    NativeEpollReactorManager manager_;
    NativeFdAuthority authority_;
    const std::uint64_t instance_;
    std::uint64_t next_generation_ = 1;
    std::array<Entry, capacity> entries_{};
    mutable std::mutex mu_{};
    bool poisoned_ = false;
};
} // namespace oreslang::runtime
