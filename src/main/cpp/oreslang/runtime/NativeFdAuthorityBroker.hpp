#pragma once
#if !defined(__linux__)
#error "NativeFdAuthorityBroker requires Linux"
#endif
#include "oreslang/runtime/NativeFdAuthority.hpp"
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <atomic>
#include <cstddef>
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
    static_assert(capacity <= UINT16_MAX, "broker slot must fit opaque token");
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
        // The trusted dispatcher must route this record to the recorded
        // authenticated owner; a bearer token alone does not identify one.
        std::uint64_t owner;
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
        if (!guard.owns_lock() || stopped_ || poisoned_ || !trusted_owner ||
                next_generation_ == UINT64_MAX) return std::nullopt;
        std::size_t slot = capacity;
        for (std::size_t i = 0; i < capacity; ++i)
            if (!entries_[i].active) { slot = i; break; }
        if (slot == capacity) return std::nullopt;
        // The authority slot and physical eventfd are minted under one
        // trusted-host nonparking transaction. No rollback can fail after
        // opening a descriptor, leaving an unowned FD or poisoned broker.
        std::optional<NativeEpollReactorManager::OwnedEventFd> owned;
        auto capability = authority_.try_mint_with(
            trusted_owner, NativeFdAuthority::readiness, [&]() noexcept {
                owned = manager_.open_owned_eventfd();
                return owned.has_value();
            });
        if (!capability) return std::nullopt;
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
        if (stopped_) return false; // no new I/O admission after shutdown
        auto* e = lookup(token, trusted_owner);
        if (!e || e->watch || e->needs_drain) return false;
        // Hold the logical authority gate before registering the kernel
        // watch. A transient gate denial leaves NO accepted epoll ticket.
        std::optional<NativeEpollReactorManager::Watch> accepted;
        if (!authority_.try_begin_watch_with(
                e->capability, trusted_owner, [&]() noexcept {
                    accepted = manager_.try_watch(e->owned, events);
                    return accepted.has_value();
                })) return false;
        e->watch = *accepted;
        return true;
    }

    bool signal(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || !e->watch ||
                authority_.phase(e->capability) != NativeFdAuthority::Phase::watching ||
                !manager_.signal_owned_eventfd(e->owned)) return false;
        // A successful signal dirties the kernel eventfd counter even if
        // a concurrent cancellation later wins the terminal outcome.
        e->needs_drain = true;
        return true;
    }

    // An eventfd stays readable until its counter is explicitly consumed.
    // The trusted owner must acknowledge a delivered READY completion before
    // registering a fresh watch; otherwise a persistent kernel counter could
    // generate spurious immediate readiness on every rearm. Never drain an
    // active watch or after revocation/shutdown.
    bool drain_eventfd(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || e->watch || !e->needs_drain ||
                authority_.phase(e->capability) != NativeFdAuthority::Phase::owned)
            return false;
        if (!manager_.drain_owned_eventfd(e->owned)) return false;
        e->needs_drain = false;
        return true;
    }

    // Cancellation is not a terminal receipt: try_pop must drain the result.
    bool try_cancel(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        // A revoked owner cannot invoke the ordinary cancellation API:
        // only trusted broker teardown may cancel revoking watches.
        return e && e->watch &&
            authority_.phase(e->capability) == NativeFdAuthority::Phase::watching &&
            manager_.try_cancel(*e->watch);
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
    // An incomplete ledger transition retains one exact pending terminal
    // instead of silently losing a consumed manager completion.
    std::optional<Completion> try_pop() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return std::nullopt;
        for (auto& e : entries_) {
            if (e.active && e.watch &&
                    authority_.phase(e.capability) ==
                        NativeFdAuthority::Phase::revoking_watch)
                (void)manager_.try_cancel(*e.watch);
        }
        // A real epoll terminal is removed from the manager once popped.
        // Preserve it until BOTH ledger transitions succeed; a nonparking
        // try_lock denial is not permission to drop terminal evidence.
        if (!pending_) {
            auto completed = manager_.try_pop();
            if (!completed) return std::nullopt;
            for (std::size_t i = 0; i < capacity; ++i) {
                const auto& e = entries_[i];
                if (e.active && e.watch &&
                        e.watch->reactor == completed->reactor &&
                        e.watch->ticket.value == completed->event.ticket.value) {
                    pending_ = Pending{i, *completed, false};
                    break;
                }
            }
            if (!pending_) {
                poisoned_ = true; // Unknown terminal never crosses owners.
                return std::nullopt;
            }
        }
        auto& e = entries_[pending_->slot];
        if (!pending_->published) {
            if (!authority_.publish_terminal(e.capability, e.owner))
                return std::nullopt; // Retain the one pending event.
            pending_->published = true;
        }
        if (!authority_.consume_terminal(e.capability, e.owner))
            return std::nullopt; // Retry without publishing twice.
        const Completion verified{e.token, e.owner,
                                  pending_->event.event.outcome,
                                  pending_->event.event.events};
        e.watch.reset();
        pending_.reset();
        return verified;
    }

    // Physical close succeeds only after the manager retires the epoll claim.
    // A failed close never produces logical reclamation or slot reuse.
    bool confirm_reclaimed(Token token, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || poisoned_) return false;
        auto* e = lookup(token, trusted_owner);
        if (!e || e->watch) return false;
        // Authenticate pending reclamation BEFORE physical close, and
        // tombstone under the same authority gate after it succeeds.
        if (!authority_.try_confirm_reclaimed_with(
                e->capability, trusted_owner, [&]() noexcept {
                    return manager_.close_owned_eventfd(e->owned);
                })) return false;
        *e = Entry{};
        return true;
    }

    // Blocking/joining teardown is trusted host-only, never an actor turn.
    void stop() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        // Publish the broker admission fence before joining reactor threads.
        // Revocation, terminal drainage, and host reclamation remain legal.
        stopped_ = true;
        manager_.stop();
    }
    std::size_t started_threads() const noexcept {
        return manager_.started_threads();
    }
#if defined(ORESLANG_NIO_TEST_HOOKS)
    // Isolated native fixture hooks; no guest or normal release ABI.
    void test_hold_authority_gate_until(std::atomic<bool>& acquired,
                                         const std::atomic<bool>& release) {
        authority_.test_hold_gate_until(acquired, release);
    }
    void test_deny_next_terminal_consume(unsigned count = 1) noexcept {
        authority_.test_deny_next_consume(count);
    }
    void test_deny_next_watch_begin(unsigned count = 1) noexcept {
        authority_.test_deny_next_begin(count);
    }
    void test_deny_next_cleanup(unsigned count = 1) noexcept {
        authority_.test_deny_next_reclaim(count);
    }
    bool test_has_pending_terminal(bool require_published = false) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        return guard.owns_lock() && pending_.has_value() &&
               (!require_published || pending_->published);
    }
#endif

private:
    struct Entry {
        bool active = false;
        Token token{};
        std::uint64_t owner = 0;
        NativeFdAuthority::Capability capability{};
        NativeEpollReactorManager::OwnedEventFd owned{};
        std::optional<NativeEpollReactorManager::Watch> watch{};
        bool needs_drain = false; // broker-owned, never guest-settable
    };
    struct Pending {
        std::size_t slot;
        NativeEpollReactorManager::Completion event;
        bool published;
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
    std::optional<Pending> pending_{};
    mutable std::mutex mu_{};
    bool poisoned_ = false;
    bool stopped_ = false; // host admission fence, protected by mu_
};
} // namespace oreslang::runtime
