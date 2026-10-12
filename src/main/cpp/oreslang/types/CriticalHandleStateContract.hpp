#pragma once

#include "oreslang/types/FunctionEffects.hpp"

#include <cstdint>

namespace oreslang {

/**
 * Native admission ABI for the sealed critical handle_state intrinsic.
 * LLVM has no full guest actor-state codegen yet. This is a shared semantic
 * contract, not a claim that actor-state guest source compiles to LLVM.
 */
enum class CriticalStateMode : std::uint8_t { None, Read, Write };

enum class CriticalStateAdmission : std::uint8_t {
    Accept,
    MissingIntrinsicCritical,
    MissingCallbackCritical,
    ReadModeCannotWrite,
    InvalidStateCallback,
    CallbackEscape,
    EffectExceedsWith,
    LoanAcrossSuspension,
    MissingContinuationFence,
    ScopeDenied
};

struct CriticalStateCallback {
    bool intrinsic_critical = true;  // sealed runtime intrinsic
    // Source spelling, independently checked against lowered state_mode:
    // an unmarked callback can never acquire WRITE admission.
    bool callback_critical = false;
    CriticalStateMode state_mode = CriticalStateMode::None;
    bool lexical = true;
    bool one_state_parameter = false;
    bool escapes = false;
    bool state_loan_live_across_suspend = false;
    bool continuation_generation_fenced = false;
    FunctionEffects inferred{};
};

inline CriticalStateAdmission admit_critical_handle_state(
        const CriticalStateCallback& callback,
        FunctionEffects with_effects,
        bool verified_serial_actor = false) noexcept {
    if (!callback.intrinsic_critical) return CriticalStateAdmission::MissingIntrinsicCritical;
    // Canonical contract: the absence of 'critical' is an immutable read
    // loan. 'critical' (Write) grants exclusive write authority. A verified
    // serial actor never upgrades an unmarked callback to mutable.
    (void)verified_serial_actor;
    const auto mode = callback.state_mode;
    // None is missing lowering metadata, NOT a synonym for READ. The
    // frontend must explicitly lower plain lambdas to Read.
    if (mode == CriticalStateMode::None
            || (mode == CriticalStateMode::Write && !callback.callback_critical)) {
        return CriticalStateAdmission::MissingCallbackCritical;
    }
    if (!callback.lexical || !callback.one_state_parameter) {
        return CriticalStateAdmission::InvalidStateCallback;
    }
    if (callback.escapes) return CriticalStateAdmission::CallbackEscape;
    // A broad with row never upgrades a read-only state capability.
    if (mode == CriticalStateMode::Read
            && callback.inferred.has(FunctionEffect::StateWrite)) {
        return CriticalStateAdmission::ReadModeCannotWrite;
    }
    if (!with_effects.permits(callback.inferred)) {
        return CriticalStateAdmission::EffectExceedsWith;
    }
    // A callback still holds its lexical state loan. It cannot suspend
    // even if some outer continuation generation is fenced; there is no
    // legal handoff of a live state reference to another actor turn.
    if (callback.inferred.has(FunctionEffect::Suspend)
            || callback.state_loan_live_across_suspend)
        return CriticalStateAdmission::LoanAcrossSuspension;
    return CriticalStateAdmission::Accept;
}

} // namespace oreslang
