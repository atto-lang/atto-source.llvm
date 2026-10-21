#pragma once

#include <stdexcept>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

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

inline llvm::Value* lower_native_entry_i64(
        llvm::IRBuilder<>& builder,
        llvm::Value* key,
        llvm::Value* value) {
    if (!key || !value
            || !key->getType()->isIntegerTy(64)
            || !value->getType()->isIntegerTy(64)) {
        throw std::invalid_argument(
            "native Entry<int,int> lowering requires two i64 values");
    }
    auto* type = native_entry_i64_type(builder.getContext());
    llvm::Value* aggregate = llvm::UndefValue::get(type);
    aggregate = builder.CreateInsertValue(
        aggregate, key, {native_entry_key_index()}, "entry.key.insert");
    return builder.CreateInsertValue(
        aggregate, value, {native_entry_value_index()}, "entry.value.insert");
}

inline llvm::Value* lower_native_entry_i64_key(
        llvm::IRBuilder<>& builder,
        llvm::Value* entry) {
    auto* expected = native_entry_i64_type(builder.getContext());
    if (!entry || entry->getType() != expected) {
        throw std::invalid_argument(
            "native Entry key projection requires nominal Entry<int,int>");
    }
    return builder.CreateExtractValue(
        entry, {native_entry_key_index()}, "entry.key");
}

inline llvm::Value* lower_native_entry_i64_value(
        llvm::IRBuilder<>& builder,
        llvm::Value* entry) {
    auto* expected = native_entry_i64_type(builder.getContext());
    if (!entry || entry->getType() != expected) {
        throw std::invalid_argument(
            "native Entry value projection requires nominal Entry<int,int>");
    }
    return builder.CreateExtractValue(
        entry, {native_entry_value_index()}, "entry.value");
}

} // namespace oreslang::backend
