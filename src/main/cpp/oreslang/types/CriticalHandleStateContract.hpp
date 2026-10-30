#pragma once

#include "oreslang/types/FunctionEffects.hpp"

#include <cstdint>

namespace oreslang {

/**
 * Native admission ABI for the sealed critical handle_state intrinsic.
 * LLVM has no full guest actor-state codegen yet. This is a shared semantic
 * contract, not a claim that actor-state guest source compiles to LLVM.
 */
enum class CriticalStateAdmission : std::uint8_t {
    Accept,
    MissingIntrinsicCritical,
    MissingCallbackCritical,
    InvalidStateCallback,
    CallbackEscape,
    EffectExceedsUsing,
    LoanAcrossSuspension,
    MissingLogicalTurnRetention,
    MissingContinuationFence,
    ScopeDenied
};

struct CriticalStateCallback {
    bool intrinsic_critical = true;  // sealed runtime intrinsic
    bool callback_critical = false;
    bool lexical = true;
    bool one_state_parameter = false;
    bool escapes = false;
    bool state_loan_live_across_suspend = false;
    bool logical_turn_retained_across_suspend = false;
    bool continuation_generation_fenced = false;
    FunctionEffects inferred{};
};

inline CriticalStateAdmission admit_critical_handle_state(
        const CriticalStateCallback& callback,
        FunctionEffects using_effects,
        bool verified_serial_actor = false) noexcept {
    if (!callback.intrinsic_critical) return CriticalStateAdmission::MissingIntrinsicCritical;
    // Scheduler-proven serial admission does not replace the mandatory
    // inline critical callback marker. Otherwise a serial actor could
    // silently authorize an unmarked state-capability callback.
    if (!callback.callback_critical)
        return CriticalStateAdmission::MissingCallbackCritical;
    // Only a verified scheduler/compiler actor context may mint state access.
    // Critical syntax by itself is not an authorization credential.
    if (!verified_serial_actor)
        return CriticalStateAdmission::ScopeDenied;
    if (!callback.lexical || !callback.one_state_parameter) {
        return CriticalStateAdmission::InvalidStateCallback;
    }
    if (callback.escapes) return CriticalStateAdmission::CallbackEscape;
    if (!using_effects.permits(callback.inferred)) {
        return CriticalStateAdmission::EffectExceedsUsing;
    }
    // An asserted cross-suspension state loan is illegal even if a forged or
    // incomplete inferred row omitted Suspend. The effect row must not erase
    // ownership evidence supplied by the compiler's lifetime checker.
    if (callback.state_loan_live_across_suspend) {
        return CriticalStateAdmission::LoanAcrossSuspension;
    }
    if (callback.inferred.has(FunctionEffect::Suspend)) {
        // Suspension releases only the physical carrier. The same logical
        // actor handler must retain the actor-wide user lane; later mail or
        // another user callback may be queued but cannot enter guest state.
        if (!callback.logical_turn_retained_across_suspend) {
            return CriticalStateAdmission::MissingLogicalTurnRetention;
        }
        if (!callback.continuation_generation_fenced) {
            return CriticalStateAdmission::MissingContinuationFence;
        }
    }
    return CriticalStateAdmission::Accept;
}

} // namespace oreslang
