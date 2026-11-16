#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/backend/NativeSourceLogManifestV1.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace oreslang {

// Typed checked source IDs live in the standalone, host-testable
// NativeSourceLogManifestV1.hpp. This codegen performs an independent
// post-frontend exact callable/site inventory check before IR publication.

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false,
                                  bool bounded_allocator_provider = false,
                                  const std::string& isolate_entry = {},
                                  bool actor_region_v1 = false,
                                  const std::vector<CheckedSourceRoutineV1>& checked_routines = {},
                                  const std::vector<CheckedLogSiteV1>& checked_sites = {});

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
