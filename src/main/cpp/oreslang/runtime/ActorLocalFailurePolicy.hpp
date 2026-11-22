#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"
#include <cstdint>

namespace oreslang {

/**
 * Host/native counterpart to Graal's actor-local uncaught-hook admission.
 * Hooks run only after lexical unwind, on the FAILING actor's admitted lane.
 * This is a routing policy substrate, not LLVM guest method codegen.
 */
enum class ActorLocalResolution : std::uint8_t { Handled, Escalate };

struct ActorLocalHookReply {
    ActorLocalResolution resolution = ActorLocalResolution::Escalate;
    // Return means callback itself succeeded. A non-returning callback effect
    // MUST NOT be silently consumed even if it requests Handled.
    EffectStatus callback_effect{};
};

using ActorLocalHook = ActorLocalHookReply (*)(void*, EffectStatus) noexcept;

// An actor's on_panic is observation only: no return value or capability to
// consume, downgrade, retry or conceal the PANIC from its parent supervisor.
// Invoke only after unwind on a PROVEN safe execution lane; do not invoke this
// on an exhausted native/host stack or from an untrusted guest callback.
using ActorLocalPanicObserver = void (*)(void*, EffectStatus) noexcept;

// Local hooks may escalate a failure, independently of lexical cleanup's
// first-terminal-wins policy. ProcessExit must never hide a failing actor.
constexpr unsigned actor_local_effect_rank(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Return:
        case EffectKind::ProcessExit: return 0;
        case EffectKind::Throw: return 1;
        case EffectKind::Raise: return 2;
        case EffectKind::Panic: return 3;
        case EffectKind::Cancel: return 4;
        case EffectKind::Fatal: return 5;
    }
    return 5;
}

struct ActorLocalDispatch {
    bool hook_invoked = false;
    bool handled = false;
    // If handled, outgoing is Return. Otherwise outgoing preserves/escalates
    // the terminal typed effect for the supervisor CONTROL path.
    EffectStatus outgoing{};
};

/**
 * Fail-closed, allocation-free effect selection. No on_panic parameter exists.
 * Panic, cancel, fatal and normal return bypass actor-local hooks completely.
 * A callback that itself panics/raises cannot rewrite a higher-priority cause
 * into a lower one. Caller enforces exclusive actor-turn affinity and ensures
 * cleanup completed before invoking this policy.
 */
inline ActorLocalDispatch dispatch_actor_local_unhandled(
        EffectStatus failure,
        void* state,
        ActorLocalHook on_throw,
        ActorLocalHook on_raise) noexcept {
    failure = normalize_effect(failure);
    ActorLocalHook hook = nullptr;
    if (failure.kind == EffectKind::Throw) hook = on_throw;
    if (failure.kind == EffectKind::Raise) hook = on_raise;
    if (hook == nullptr) return {false, false, failure};

    const ActorLocalHookReply reply = hook(state, failure);
    if (reply.callback_effect.kind == EffectKind::Return
            && reply.resolution == ActorLocalResolution::Handled) {
        return {true, true, {EffectKind::Return, 0}};
    }

    EffectStatus terminal = failure;
    const EffectStatus callback = normalize_effect(reply.callback_effect);
    if (actor_local_effect_rank(callback.kind) > actor_local_effect_rank(failure.kind)) {
        terminal = callback;
    }
    return {true, false, terminal};
}

/**
 * Optional non-recovering on_panic observer after lexical cleanup.
 * The caller is responsible for verifying an independent, stack-safe lane and
 * for publishing the original panic on the parent's priority CONTROL path
 * regardless of observer availability. Skipping an unsafe observer must never
 * skip supervisor delivery. No heap allocation or guest method lowering here.
 */
inline ActorLocalDispatch observe_actor_panic_on_safe_lane(
        EffectStatus failure,
        void* state,
        ActorLocalPanicObserver on_panic) noexcept {
    failure = normalize_effect(failure);
    if (failure.kind != EffectKind::Panic || on_panic == nullptr)
        return {false, false, failure};
    on_panic(state, failure);
    // The original panic is immutable for supervision, even if the observer
    // records diagnostics. No callback result exists that could mark Handled.
    return {true, false, failure};
}

} // namespace oreslang
