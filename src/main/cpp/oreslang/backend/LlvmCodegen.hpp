#pragma once

#include "oreslang/frontend/Reachability.hpp"
#include <string>

namespace oreslang {

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(
        const Program& source,
        bool wasm32_target = false,
        bool cortex_m_target = false,
        const RuntimeFeatureSet* expected_runtime_features = nullptr);

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
