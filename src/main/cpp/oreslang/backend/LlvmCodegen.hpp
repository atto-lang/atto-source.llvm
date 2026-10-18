#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/NativeEnumAotSlice.hpp"
#include <string>

namespace oreslang {

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false);

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);


// Compile the deliberately restricted checked enum/case guest source slice
// into verified LLVM IR with direct eos2 statically selected call symbols.
// Does not admit the full language, virtual methods, imports or actor enums.
std::string emit_verified_enum_aot_ir(const enum_aot::Plan& plan);

} // namespace oreslang
