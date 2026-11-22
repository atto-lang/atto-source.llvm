#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <string>

namespace oreslang {

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false,
                                  bool bounded_allocator_provider = false,
                                  bool tenant_vm_guest = false,
                                  // Hosted Linux cross triple (TargetAbi experimental-hosted-linux-ir),
                                  // or nullptr for the historical inferred host target.
                                  const char* hosted_linux_triple = nullptr);

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
