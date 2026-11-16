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
    c.inferred = row(READ | WRITE);
    const auto with_rw = row(READ | WRITE);
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    // The serial actor base is an immutable compiler property, but an
    // implicit critical callback is safe only with an actual lifetime lease.
    c.actor_is_serial = true;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.serial_lifetime_admission_proven = true;
    // Serial does not grant implicit WRITE; explicit critical is required.
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.actor_is_serial = false;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.serial_lifetime_admission_proven = false;
    // Trusted three-argument policy: a verified serial actor (actor
    // declaration + scheduler, never callback metadata) still cannot admit an
    // unmarked state write (spec ch.05/ch.22).
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.callback_critical = true;
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::Accept);
    c.intrinsic_critical = false;
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::MissingIntrinsicCritical);
    c.intrinsic_critical = true;
    c.one_state_parameter = false;
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::InvalidStateCallback);
    c.one_state_parameter = true;
    c.lexical = false;
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::InvalidStateCallback);
    c.lexical = true;
    c.escapes = true;
    CHECK(admit_critical_handle_state(c, with_rw, true)
            == CriticalStateAdmission::CallbackEscape);
    c.escapes = false;
    CHECK(admit_critical_handle_state(c, row(READ), true)
            == CriticalStateAdmission::EffectExceedsWith);
    // Serial admission cannot be inferred from the callback itself; the
    // two-argument untrusted policy still fails closed for unmarked writes.
    c.callback_critical = false;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingCallbackCritical);
    c.callback_critical = true;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::Accept);

    c.actor_is_serial = true;
    c.serial_lifetime_admission_proven = true;
    c.callback_critical = false;
    c.intrinsic_critical = false;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::MissingIntrinsicCritical);
    c.intrinsic_critical = true;

    // Unmarked callbacks are READ-only, for ordinary and serial actors alike.
    c.inferred = row(READ);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);
    c.actor_is_serial = false;
    c.serial_lifetime_admission_proven = false;
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);
    c.callback_critical = true;
    c.inferred = row(READ | WRITE);

    c.lexical = false;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::InvalidStateCallback);
    c.lexical = true;
    c.one_state_parameter = false;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::InvalidStateCallback);
    c.one_state_parameter = true;

    c.escapes = true;
    CHECK(admit_critical_handle_state(c, with_rw)
            == CriticalStateAdmission::CallbackEscape);
    c.escapes = false;

    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::EffectExceedsWith);
    c.inferred = row(READ);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);

    c.inferred = row(READ | IO);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(READ | IO))
            == CriticalStateAdmission::Accept);

    c.inferred = row(READ | SUSPEND);
    CHECK(admit_critical_handle_state(c, row(READ | SUSPEND))
            == CriticalStateAdmission::MissingContinuationFence);
    c.continuation_generation_fenced = true;
    CHECK(admit_critical_handle_state(c, row(READ | SUSPEND))
            == CriticalStateAdmission::Accept);
    c.state_loan_live_across_suspend = true;
    CHECK(admit_critical_handle_state(c, row(READ | SUSPEND))
            == CriticalStateAdmission::LoanAcrossSuspension);
    c.state_loan_live_across_suspend = false;
    CHECK(admit_critical_handle_state(c, row(READ | SUSPEND))
            == CriticalStateAdmission::Accept);

    // Trusted admission must not accept forged/reserved bits in either the
    // inferred callback row or the caller's upper-bound "with" row.
    constexpr std::uint32_t UNKNOWN = 1u << 31;
    c.inferred = row(READ);
    CHECK(admit_critical_handle_state(c, row(READ | UNKNOWN))
            == CriticalStateAdmission::EffectExceedsWith);
    c.inferred = row(READ | UNKNOWN);
    CHECK(admit_critical_handle_state(c, row(READ | UNKNOWN))
            == CriticalStateAdmission::EffectExceedsWith);
    CHECK(admit_critical_handle_state(c, row(READ | SUSPEND))
            == CriticalStateAdmission::EffectExceedsWith);
    c.inferred = row(READ);
    CHECK(admit_critical_handle_state(c, row(READ))
            == CriticalStateAdmission::Accept);
    return 0;
}
