#include "oreslang/types/CriticalHandleStateContract.hpp"

// Scalar proof interface only; all policy decisions come from the unchanged
// production header. The verifier rejects LLVM instructions it cannot model.
extern "C" unsigned state_admits(unsigned char mode, unsigned char flags,
                                  unsigned allowed, unsigned inferred,
                                  unsigned char serial) {
    oreslang::CriticalStateCallback callback{};
    callback.state_mode = static_cast<oreslang::CriticalStateMode>(mode);
    callback.intrinsic_critical = (flags & 1) != 0;
    callback.callback_critical = (flags & 2) != 0;
    callback.lexical = (flags & 4) != 0;
    callback.one_state_parameter = (flags & 8) != 0;
    callback.escapes = (flags & 16) != 0;
    callback.state_loan_live_across_suspend = (flags & 32) != 0;
    callback.continuation_generation_fenced = (flags & 64) != 0;
    callback.inferred.bits = inferred;
    return oreslang::admit_critical_handle_state(callback, {allowed}, serial != 0)
            == oreslang::CriticalStateAdmission::Accept;
}
