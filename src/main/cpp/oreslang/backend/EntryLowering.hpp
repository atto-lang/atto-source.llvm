#pragma once

#include <stdexcept>

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"

namespace oreslang::backend {

/**
 * Canonical first LLVM representation for nominal Entry<int,int>.
 *
 * Use an identified struct, never a literal {i64,i64}: literal LLVM structs
 * are structural and would let Tuple/record layouts impersonate Entry.
 */
inline llvm::StructType* native_entry_i64_type(llvm::LLVMContext& context) {
    constexpr const char* kName = "ores.entry.i64.i64";
    if (auto* existing = llvm::StructType::getTypeByName(context, kName)) {
        if (existing->isOpaque()
                || existing->isPacked()
                || existing->getNumElements() != 2
                || !existing->getElementType(0)->isIntegerTy(64)
                || !existing->getElementType(1)->isIntegerTy(64)) {
            throw std::runtime_error(
                "native Entry<int,int> LLVM type identity collision");
        }
        return existing;
    }

    auto* i64 = llvm::Type::getInt64Ty(context);
    return llvm::StructType::create(context, {i64, i64}, kName, false);
}

inline constexpr unsigned native_entry_key_index() noexcept { return 0; }
inline constexpr unsigned native_entry_value_index() noexcept { return 1; }

} // namespace oreslang::backend
