#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <string>

namespace oreslang {

// Deliberately limited experimental XDP entrypoint. This is NOT a general
// eBPF verifier proof, map/helper API, or kernel-loading capability.
std::string emit_verified_bpf_xdp_ir(const Program& source);

} // namespace oreslang
