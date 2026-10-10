#pragma once

#include "oreslang/frontend/Parser.hpp"

namespace oreslang {

// Enforces explicit ownership operations in the LLVM i64-only source subset.
// Borrowed references, pointer projections, actor hops and suspension are
// rejected by the frontend before this pass; none has a native ABI yet.
void validate_i64_ownership(const Program& program);

} // namespace oreslang
