#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {
// Runtime-private, same-process HTTP ownership fence. A token is mailbox DATA,
// never a raw FD/pointer or wire capability. Storage must outlive scheduler drain.
// One producer (reactor), one admitted actor consumer, one completion consumer.
// Non-atomic request/response storage may be touched ONLY by its phase's owner.
class NativeHttpExchange final {
public:
    enum class Phase : unsigned char { reactor, queued, actor, completed };
    struct Token { std::uint64_t generation, actor; };
    bool enqueue(std::uint64_t actor) noexcept {
        if (phase() != Phase::reactor || !actor
                || (state_.load(std::memory_order_relaxed) >> 2) == (std::numeric_limits<std::uint64_t>::max() >> 2)) return false;
        const auto generation = (state_.load(std::memory_order_relaxed) >> 2) + 1;
        actor_.store(actor, std::memory_order_relaxed);
        state_.store((generation << 2) | static_cast<unsigned>(Phase::queued), std::memory_order_release);
        return true;
    }
    // Only reactor, after mailbox rejection; rejected sends cannot run a callback.
    bool rollback(Token token) noexcept { return transition(token, Phase::queued, Phase::reactor); }
    // Caller must first authenticate scheduler-current-actor identity.
    bool claim(Token token) noexcept { return transition(token, Phase::queued, Phase::actor); }
    bool complete(Token token) noexcept { return transition(token, Phase::actor, Phase::completed); }
    bool reclaim(Token token) noexcept { return transition(token, Phase::completed, Phase::reactor); }
    Phase phase() const noexcept { return static_cast<Phase>(state_.load(std::memory_order_acquire) & 3); }
    Token token() const noexcept { return {state_.load(std::memory_order_acquire) >> 2, actor_.load(std::memory_order_relaxed)}; } // owner or published mailbox consumer only
private:
    bool transition(Token token, Phase from, Phase to) noexcept {
        const auto expected_state = (token.generation << 2) | static_cast<unsigned>(from);
        if (state_.load(std::memory_order_acquire) != expected_state) return false;
        if (token.actor != actor_.load(std::memory_order_acquire) || !token.generation
                || token.generation > (std::numeric_limits<std::uint64_t>::max() >> 2)) return false;
        auto expected = (token.generation << 2) | static_cast<unsigned>(from);
        return state_.compare_exchange_strong(expected,
            (token.generation << 2) | static_cast<unsigned>(to), std::memory_order_acq_rel);
    }
    std::atomic<std::uint64_t> state_{0}, actor_{0};
};
} // namespace oreslang::runtime
