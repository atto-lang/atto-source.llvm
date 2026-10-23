#pragma once
#include <string>

namespace oreslang {

// Runs compiler-generated, verified host IR through LLVM ORC LLJIT.
// Trusted host only; returns the generated native i32 main() exit status.
int execute_host_llvm_ir_with_orc(const std::string& verified_ir);

} // namespace oreslang
