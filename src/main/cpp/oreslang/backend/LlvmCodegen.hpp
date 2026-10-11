#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <string>

namespace oreslang {

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source);

struct ClientContractHeader;
// Emit LLVM-verified, signature-only String ABI declarations. These declarations
// are NOT executable implementations or runtime admission evidence.
std::string emit_verified_client_contract_llvm_ir(const ClientContractHeader& source);

} // namespace oreslang
