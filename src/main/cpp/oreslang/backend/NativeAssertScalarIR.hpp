#pragma once

#include <string>

namespace oreslang {

// LLVM code generation of the primitive ABI v0 *substrate*. The resulting
// functions are NOT yet linked to the Oreslang source-owned std/assert module.
// Scalar/BoolResult byte layout and source failure/Option channels are tested
// by compiling emitted IR and comparing against NativeAssertScalarAbi.hpp.
std::string emit_native_assert_scalar_llvm_ir_v0();

} // namespace oreslang
