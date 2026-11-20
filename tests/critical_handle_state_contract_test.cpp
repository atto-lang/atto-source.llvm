#include "oreslang/types/CriticalHandleStateContract.hpp"

#include <stdexcept>
using namespace oreslang;
#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

static FunctionEffects row(std::uint32_t bits) noexcept { return {bits}; }
static constexpr std::uint32_t READ = static_cast<std::uint32_t>(FunctionEffect::StateRead);
static constexpr std::uint32_t WRITE = static_cast<std::uint32_t>(FunctionEffect::StateWrite);
static constexpr std::uint32_t SUSPEND = static_cast<std::uint32_t>(FunctionEffect::Suspend);
static constexpr std::uint32_t IO = static_cast<std::uint32_t>(FunctionEffect::Io);

int main() {
    CriticalStateCallback c{};
    c.one_state_parameter = true;
    c.inferred = row(READ);
    // No compiler-authored lowered mode is an admission failure.
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::MissingCallbackCritical);
    c.state_mode = CriticalStateMode::Read;
    const auto using_rw = row(READ | WRITE);
    // A correctly declared read capability cannot pass native admission
    // without compiler/runtime-verified continuation-generation fencing.
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::MissingContinuationFence);
    CHECK(admit_critical_handle_state(c, using_rw, true)
            == CriticalStateAdmission::MissingContinuationFence);
    c.continuation_generation_fenced = true;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::Accept); // unmarked callback is immutable read
    CHECK(admit_critical_handle_state(c, using_rw, true)
            == CriticalStateAdmission::Accept); // serial does not upgrade to write
    c.inferred = row(READ | WRITE);
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::ReadModeCannotWrite);
    c.inferred = row(READ);

    c.state_mode = CriticalStateMode::Read;
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);
    CHECK(admit_critical_handle_state(c, row(WRITE))
            == CriticalStateAdmission::EffectExceedsWith);

    c.inferred = row(READ | WRITE);
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::ReadModeCannotWrite); // using cannot upgrade read
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | IO))
            == CriticalStateAdmission::ReadModeCannotWrite);

    c.state_mode = CriticalStateMode::Write;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.callback_critical = true;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::Accept);
    CHECK(admit_critical_handle_state(c, row(WRITE))
            == CriticalStateAdmission::EffectExceedsWith);
    c.inferred = row(WRITE); // pure overwrite is not an implicit read
    CHECK(admit_critical_handle_state(c, row(WRITE))
            == CriticalStateAdmission::Accept);

    c.intrinsic_critical = false;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::MissingIntrinsicCritical);
    c.intrinsic_critical = true;
    c.one_state_parameter = false;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::InvalidStateCallback);
    c.one_state_parameter = true;
    c.lexical = false;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::InvalidStateCallback);
    c.lexical = true;
    c.escapes = true;
    CHECK(admit_critical_handle_state(c, using_rw)
            == CriticalStateAdmission::CallbackEscape);
    c.escapes = false;

    c.inferred = row(READ | IO);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(READ | IO))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | IO))
            == CriticalStateAdmission::Accept);

    c.inferred = row(READ | SUSPEND);
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | SUSPEND))
            == CriticalStateAdmission::LoanAcrossSuspension);
    c.continuation_generation_fenced = true;
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | SUSPEND))
            == CriticalStateAdmission::LoanAcrossSuspension);
    c.state_loan_live_across_suspend = true;
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | SUSPEND))
            == CriticalStateAdmission::LoanAcrossSuspension);
    c.state_loan_live_across_suspend = false;
    CHECK(admit_critical_handle_state(c, row(READ | WRITE | SUSPEND))
            == CriticalStateAdmission::LoanAcrossSuspension);

    // Neither out-of-range enum modes nor future/unknown effect bits can
    // authorize a state loan through an older native runtime.
    c.inferred = row(READ);
    c.state_mode = static_cast<CriticalStateMode>(255);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::InvalidStateCallback);
    c.state_mode = CriticalStateMode::Read;
    constexpr std::uint32_t UNKNOWN = 1u << 30;
    CHECK(!row(UNKNOWN).valid());
    CHECK(admit_critical_handle_state(c, row(READ | UNKNOWN))
            == CriticalStateAdmission::InvalidEffectMetadata);
    c.inferred = row(READ | UNKNOWN);
    CHECK(admit_critical_handle_state(c, row(READ | UNKNOWN))
            == CriticalStateAdmission::InvalidEffectMetadata);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::InvalidEffectMetadata);

    // Acquiring a state capability is an effect even when the callback
    // infers no reads or writes. Runtime metadata must not mint authority.
    c.inferred = row(0);
    c.state_mode = CriticalStateMode::Read;
    c.callback_critical = false;
    CHECK(admit_critical_handle_state(c, row(WRITE))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);
    c.state_mode = CriticalStateMode::Write;
    c.callback_critical = true;
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(WRITE))
            == CriticalStateAdmission::Accept);

    return 0;
}
