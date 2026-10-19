#pragma once
#include "oreslang/frontend/Parser.hpp"
#include <string>
namespace oreslang {
// Experimental LLVM IR output, requires independent BPF codegen/verifier gate.
std::string emit_experimental_ebpf_xdp_ir(const Program& source);
} // namespace oreslang
