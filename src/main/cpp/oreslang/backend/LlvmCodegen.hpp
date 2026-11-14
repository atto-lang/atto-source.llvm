#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace oreslang {

// An exact source-controlled routine ID is assigned by the caller only after
// its own registry/schema admission. Compiler checks the correspondence to
// THIS native frontend's validated function declarations before emitting IR.
// This is lexical metadata only, not a runtime probe or an ORC-generated JIT.
struct CheckedSourceRoutineV1 {
    std::string symbol;
    std::size_t arity;
    std::string routine_id;
};

// Literal-only compiled log source site: (function,arity,message) is an exact
// temporary binding key, NOT the persistent site identity. Site IDs are
// supplied by the source-controlled manifest and never computed at runtime.
struct CheckedLogSiteV1 {
    std::string symbol;
    std::size_t arity;
    std::string message;
    std::string routine_id;
    std::string site_id;
};

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false,
                                  bool bounded_allocator_provider = false,
                                  const std::string& isolate_entry = {},
                                  const std::vector<CheckedSourceRoutineV1>& checked_routines = {},
                                  const std::vector<CheckedLogSiteV1>& checked_sites = {});

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
