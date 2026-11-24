#pragma once

#include <string>
#include <string_view>

#include "oreslang/frontend/Parser.hpp"

namespace oreslang {

// Validate all native v1 model declarations, compute canonical graph identities,
// and lower direct Model(config...).run(input) expressions into ordinary verified
// scalar function calls. No runtime graph interpreter or hidden native math.
void admit_and_lower_models(Program& program);

// Exposed for deterministic cross-backend golden tests.
std::string model_sha256(std::string_view input);

} // namespace oreslang
