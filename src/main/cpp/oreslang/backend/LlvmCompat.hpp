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
#if LLVM_VERSION_MAJOR >= 23
    (void)pointee;
    return llvm::PointerType::get(context, 0);
#else
    (void)context;
    return pointee->getPointerTo();
#endif
}

inline void native_lifetime_start(llvm::IRBuilder<>& builder, llvm::Value* ptr,
                                  std::uint64_t bytes) {
#if LLVM_VERSION_MAJOR >= 23
    (void)bytes;
    builder.CreateLifetimeStart(ptr);
#else
    builder.CreateLifetimeStart(ptr, llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(builder.getContext()), bytes));
#endif
}
inline void native_lifetime_end(llvm::IRBuilder<>& builder, llvm::Value* ptr,
                                std::uint64_t bytes) {
#if LLVM_VERSION_MAJOR >= 23
    (void)bytes;
    builder.CreateLifetimeEnd(ptr);
#else
    builder.CreateLifetimeEnd(ptr, llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(builder.getContext()), bytes));
#endif
}

// Centralize the LLVM 19/20+ intrinsic API transition. Checked arithmetic
// requires one overload type; trap requires none. Keep the choice out of
// guest lowering so newly added intrinsics do not regress newer LLVM builds.
inline llvm::Function* native_intrinsic(llvm::Module& module,
                                        llvm::Intrinsic::ID id,
                                        llvm::ArrayRef<llvm::Type*> overloads = {}) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(&module, id, overloads);
#else
    return llvm::Intrinsic::getDeclaration(&module, id, overloads);
#endif
}

inline llvm::Function* native_trap(llvm::Module& module) {
    return native_intrinsic(module, llvm::Intrinsic::trap);
}
}  // namespace oreslang
