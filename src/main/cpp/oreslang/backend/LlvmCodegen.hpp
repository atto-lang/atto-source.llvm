#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <string>

namespace oreslang {

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false,
                                  bool bounded_allocator_provider = false,
                                  const std::string& isolate_entry = {});

// Restricted IR-only experiment: lower the single copied-i64 actor Task
// producer into a checked (status, out) function without actor bootstrap,
// Task scheduling, await resumption, or guest-facing runtime authority.
// Explicitly NOT a runnable actor Task program.
std::string emit_native_actor_task_i64_producer_ir(const Program& source);

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
