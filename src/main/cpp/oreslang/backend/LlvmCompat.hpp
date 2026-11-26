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
    // All supported LLVM versions (19+) use opaque pointers.
    (void)pointee;
    return llvm::PointerType::get(context, 0);
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

inline llvm::Function* native_intrinsic(llvm::Module& module, llvm::Intrinsic::ID id,
                                       llvm::ArrayRef<llvm::Type*> types = {}) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(&module, id, types);
#else
    return llvm::Intrinsic::getDeclaration(&module, id, types);
#endif
}

inline llvm::Function* native_trap(llvm::Module& module) {
    return native_intrinsic(module, llvm::Intrinsic::trap);
}
}  // namespace oreslang
