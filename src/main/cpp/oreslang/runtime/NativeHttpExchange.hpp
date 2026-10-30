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
        // Weak value, not a retention lease or actor authority.
        std::uint64_t request_context = 0;
        std::uint64_t request_registry = 0; // generation-scoped table identity
        std::uint64_t dispatch_ticket = 0; // actual scheduler DATA payload
    };

    bool enqueue(OresNativeActorRef actor, std::uint64_t request_context = 0,
                 std::uint64_t request_registry = 0,
                 std::uint64_t dispatch_ticket = 0) noexcept {
        if (phase() != Phase::reactor
                || pin_generation_.load(std::memory_order_acquire) != 0
                || bool(request_context) != bool(request_registry)
                || bool(request_context) != bool(dispatch_ticket)
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
        request_context_.store(request_context, std::memory_order_relaxed);
        request_registry_.store(request_registry, std::memory_order_relaxed);
        dispatch_ticket_.store(dispatch_ticket, std::memory_order_relaxed);
        submission_generation_.store(0, std::memory_order_relaxed);
        initial_entry_generation_.store(0, std::memory_order_relaxed);
        state_.store(
            (generation << 2) | static_cast<unsigned>(Phase::queued),
            std::memory_order_release);
        return true;
    }

    // Only reactor, after mailbox rejection; rejected sends cannot run a callback.
    bool rollback(Token token) noexcept {
        return transition(token, Phase::queued, Phase::reactor);
    }

    // Compatibility for context-free legacy callers. A request-bearing token
    // must NOT claim an exchange from a reactor/producer or foreign actor.
    bool claim(Token token) noexcept {
        return !token.request_context && !token.dispatch_ticket
            && transition(token, Phase::queued, Phase::actor);
    }
    // DATA admission is authenticated by the scheduler, not a caller-provided
    // actor tuple or a thread-local lookup. Never invoked on a producer thread.
    bool claim_admitted(OresNativeActorScheduler* scheduler, Token token) noexcept {
        if (!token.request_context || !token.request_registry ||
            !token.dispatch_ticket ||
            pin_generation_.load(std::memory_order_acquire)!=token.generation ||
            !scheduler ||
            !ores_native_actor_scheduler_is_current_actor(scheduler,token.actor))
            return false;
        std::uint64_t delivered=0;
        return ores_native_actor_scheduler_current_data_payload(
                   scheduler,token.actor,&delivered)
            && delivered==token.dispatch_ticket
            && transition(token, Phase::queued, Phase::actor);
    }
    // Admit initial handler once per exact exchange generation. The runtime
    // checks live scheduler DATA authority before invoking this gate; a
    // separately validated continuation uses its own one-shot resume path.
    bool enter_initial(Token token) const noexcept {
        if (!token.generation || !token.dispatch_ticket ||
            pin_generation_.load(std::memory_order_acquire)!=token.generation ||
            phase()!=Phase::actor) return false;
        const auto actual=this->token();
        if (actual.generation!=token.generation ||
            actual.dispatch_ticket!=token.dispatch_ticket ||
            actual.request_context!=token.request_context ||
            actual.request_registry!=token.request_registry ||
            actual.actor.scheduler_domain!=token.actor.scheduler_domain ||
            actual.actor.actor_slot!=token.actor.actor_slot ||
            actual.actor.generation!=token.actor.generation) return false;
        std::uint64_t expected=0;
        return initial_entry_generation_.compare_exchange_strong(
            expected,token.generation,std::memory_order_acq_rel);
    }

    bool complete(Token token) noexcept {
        return transition(token, Phase::actor, Phase::completed);
    }

    bool reclaim(Token token) noexcept {
        return transition(token, Phase::completed, Phase::reactor);
    }

    // Only one trusted producer can reserve this request's initial DATA send.
    // The reservation is never released within this exchange generation.
    bool reserve_submission(Token request) noexcept {
        if (!request.generation || !request.dispatch_ticket ||
            !request.request_context || !request.request_registry ||
            phase()!=Phase::queued ||
            pin_generation_.load(std::memory_order_acquire)!=request.generation)
            return false;
        const auto actual=token();
        if(actual.generation!=request.generation ||
           actual.request_context!=request.request_context ||
           actual.request_registry!=request.request_registry ||
           actual.dispatch_ticket!=request.dispatch_ticket ||
           actual.actor.scheduler_domain!=request.actor.scheduler_domain ||
           actual.actor.actor_slot!=request.actor.actor_slot ||
           actual.actor.generation!=request.actor.generation)
            return false;
        std::uint64_t expected=0;
        return submission_generation_.compare_exchange_strong(
            expected,request.generation,std::memory_order_acq_rel);
    }

    // A trusted physical context/body owner registers its generation before
    // releasing the reactor's dispatch ownership. Until that pin retires,
    // enqueue() cannot reuse this exchange even after complete/reclaim.
    bool bind_physical_pin(Token token) noexcept {
        if (!token.request_context || !token.request_registry
                || !token.dispatch_ticket || !token.generation
                || phase()!=Phase::queued)
            return false;
        const auto now=this->token();
        if(now.generation!=token.generation ||
                now.request_context!=token.request_context ||
                now.request_registry!=token.request_registry ||
                now.dispatch_ticket!=token.dispatch_ticket ||
                now.actor.scheduler_domain!=token.actor.scheduler_domain ||
                now.actor.actor_slot!=token.actor.actor_slot ||
                now.actor.generation!=token.actor.generation)return false;
        std::uint64_t empty=0;
        return pin_generation_.compare_exchange_strong(
            empty,token.generation,std::memory_order_acq_rel);
    }
    // Called only by the owning ExchangePin after the real completion/
    // rollback phase reaches reactor. Exact generation prevents stale release.
    bool release_physical_pin(Token token) noexcept {
        if(!token.generation || phase()!=Phase::reactor ||
                (state_.load(std::memory_order_acquire)>>2)!=token.generation)
            return false;
        std::uint64_t expected=token.generation;
        return pin_generation_.compare_exchange_strong(
            expected,0,std::memory_order_acq_rel);
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
                actor_generation_.load(std::memory_order_relaxed)},
            request_context_.load(std::memory_order_relaxed),
            request_registry_.load(std::memory_order_relaxed),
            dispatch_ticket_.load(std::memory_order_relaxed)};
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
                    != actor_generation_.load(std::memory_order_acquire)
                || token.request_context
                    != request_context_.load(std::memory_order_acquire)
                || token.request_registry
                    != request_registry_.load(std::memory_order_acquire)
                || token.dispatch_ticket
                    != dispatch_ticket_.load(std::memory_order_acquire))
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
    std::atomic<std::uint64_t> request_context_{0};
    std::atomic<std::uint64_t> request_registry_{0};
    std::atomic<std::uint64_t> dispatch_ticket_{0};
    std::atomic<std::uint64_t> pin_generation_{0};
    std::atomic<std::uint64_t> submission_generation_{0};
    mutable std::atomic<std::uint64_t> initial_entry_generation_{0};
};
} // namespace oreslang::runtime
