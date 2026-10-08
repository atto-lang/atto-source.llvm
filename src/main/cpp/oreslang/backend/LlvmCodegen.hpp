#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <string>

namespace oreslang {
// Construct LLVM IR and run LLVM verifyModule before returning any IR text.
std::string emit_verified_llvm_ir(const ConstantFunction& source);
} // namespace oreslang
