#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include "oreslang/runtime/NativeActorScheduler.h"

namespace oreslang::runtime {
// Runtime-private, same-process HTTP ownership fence. A token is mailbox DATA,
// never a raw FD/pointer or wire capability. Storage must outlive scheduler drain.
// One producer (reactor), one admitted actor consumer, one completion consumer.
// Non-atomic request/response storage may be touched ONLY by its phase's owner.
//
// Exchange generation prevents a stale token from claiming a later request.
// OresNativeActorRef independently fences actor-incarnation reuse: a later actor
// in the same numeric slot is never the owner of an earlier exchange phase.
class NativeHttpExchange final {
public:
    enum class Phase : unsigned char { reactor, queued, actor, completed };
    struct Token {
        std::uint64_t generation;
        OresNativeActorRef actor;
    };

    bool enqueue(OresNativeActorRef actor) noexcept {
        if (phase() != Phase::reactor
                || !actor.scheduler_domain
                || !actor.generation
                || (state_.load(std::memory_order_relaxed) >> 2)
                    == (std::numeric_limits<std::uint64_t>::max() >> 2))
            return false;

        const auto generation =
            (state_.load(std::memory_order_relaxed) >> 2) + 1;
        actor_domain_.store(actor.scheduler_domain, std::memory_order_relaxed);
        actor_slot_.store(actor.actor_slot, std::memory_order_relaxed);
        actor_generation_.store(actor.generation, std::memory_order_relaxed);
        state_.store(
            (generation << 2) | static_cast<unsigned>(Phase::queued),
            std::memory_order_release);
        return true;
    }

    // Only reactor, after mailbox rejection; rejected sends cannot run a callback.
    bool rollback(Token token) noexcept {
        return transition(token, Phase::queued, Phase::reactor);
    }

    // Caller must first authenticate scheduler-current-actor identity.
    bool claim(Token token) noexcept {
        return transition(token, Phase::queued, Phase::actor);
    }

    bool complete(Token token) noexcept {
        return transition(token, Phase::actor, Phase::completed);
    }

    bool reclaim(Token token) noexcept {
        return transition(token, Phase::completed, Phase::reactor);
    }

    Phase phase() const noexcept {
        return static_cast<Phase>(
            state_.load(std::memory_order_acquire) & 3);
    }

    Token token() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return Token{
            state >> 2,
            OresNativeActorRef{
                actor_domain_.load(std::memory_order_relaxed),
                actor_slot_.load(std::memory_order_relaxed),
                actor_generation_.load(std::memory_order_relaxed)}};
    }

private:
    bool transition(Token token, Phase from, Phase to) noexcept {
        if (!token.generation
                || token.generation >
                    (std::numeric_limits<std::uint64_t>::max() >> 2)
                || !token.actor.scheduler_domain
                || !token.actor.generation)
            return false;

        const auto expected_state =
            (token.generation << 2) | static_cast<unsigned>(from);
        if (state_.load(std::memory_order_acquire) != expected_state)
            return false;

        if (token.actor.scheduler_domain
                    != actor_domain_.load(std::memory_order_acquire)
                || token.actor.actor_slot
                    != actor_slot_.load(std::memory_order_acquire)
                || token.actor.generation
                    != actor_generation_.load(std::memory_order_acquire))
            return false;

        auto expected = expected_state;
        return state_.compare_exchange_strong(
            expected,
            (token.generation << 2) | static_cast<unsigned>(to),
            std::memory_order_acq_rel);
    }

    std::atomic<std::uint64_t> state_{0};
    std::atomic<std::uint64_t> actor_domain_{0};
    std::atomic<std::uint64_t> actor_slot_{0};
    std::atomic<std::uint64_t> actor_generation_{0};
};
} // namespace oreslang::runtime
