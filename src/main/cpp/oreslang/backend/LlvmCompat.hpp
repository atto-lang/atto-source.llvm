#pragma once
// LLVM 19-23 abstraction for pointer and lifetime intrinsics.
// The language/ABI does not depend on which LLVM API spells these operations.
#include <cstdint>
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

namespace oreslang {
inline llvm::Type* native_pointer_type(llvm::LLVMContext& context, llvm::Type* pointee) {
#if LLVM_VERSION_MAJOR >= 22
    (void)pointee;
    return llvm::PointerType::get(context, 0);
#else
    return pointee->getPointerTo();
#endif
}

inline void native_lifetime_start(llvm::IRBuilder<>& builder, llvm::Value* ptr,
                                  std::uint64_t bytes) {
#if LLVM_VERSION_MAJOR >= 22
    (void)bytes;
    builder.CreateLifetimeStart(ptr);
#else
    builder.CreateLifetimeStart(ptr, llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(builder.getContext()), bytes));
#endif
}
inline void native_lifetime_end(llvm::IRBuilder<>& builder, llvm::Value* ptr,
                                std::uint64_t bytes) {
#if LLVM_VERSION_MAJOR >= 22
    (void)bytes;
    builder.CreateLifetimeEnd(ptr);
#else
    builder.CreateLifetimeEnd(ptr, llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(builder.getContext()), bytes));
#endif
}

// LLVM 20+ renamed the intrinsic lookup interface. Keep the call sites
// version-neutral, including overloaded checked-arithmetic intrinsics.
inline llvm::Function* native_intrinsic_declaration(
        llvm::Module* module, llvm::Intrinsic::ID operation,
        llvm::ArrayRef<llvm::Type*> overload_types = {}) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(
        module, operation, overload_types);
#else
    return llvm::Intrinsic::getDeclaration(module, operation, overload_types);
#endif
}

inline llvm::Function* native_trap(llvm::Module& module) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(&module, llvm::Intrinsic::trap);
#else
    return llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::trap);
#endif
}
}  // namespace oreslang
