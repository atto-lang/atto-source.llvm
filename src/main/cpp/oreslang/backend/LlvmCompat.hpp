#pragma once
// LLVM 14-23 compatibility for native pointers, lifetimes, and intrinsics.
// The language/ABI does not depend on which LLVM API spells these operations.
#include <cstdint>
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

namespace oreslang {
// During IR construction a block may be empty or contain ordinary
// instructions but no terminator yet. LLVM 23 getTerminator() assumes
// well-formedness and asserts; in release builds probing it can mistake
// a non-terminator for a completed block and skip live statements.
inline bool native_block_has_terminator(const llvm::BasicBlock* block) {
    return block && !block->empty() && block->back().isTerminator();
}

inline llvm::Type* native_pointer_type(llvm::LLVMContext& context, llvm::Type* pointee) {
    // Pointers are opaque from LLVM 17: getPointerTo() already returned this
    // `ptr` type, and it is deprecated (-Werror) from LLVM 21.
#if LLVM_VERSION_MAJOR >= 17
    (void)pointee;
    return llvm::PointerType::get(context, 0);
#else
    (void)context;
    return pointee->getPointerTo();
#endif
}

// CreateGlobalStringPtr is unavailable in LLVM 23. Preserve its original
// first-byte pointer contract for older typed-pointer and modern opaque-pointer
// builds. CreateGlobalString retains the NUL terminator, private linkage and
// constant unnamed address; no runtime allocation or instruction is introduced.
inline llvm::Constant* native_global_string_ptr(llvm::IRBuilder<>& builder,
                                                llvm::StringRef bytes,
                                                const llvm::Twine& name) {
    auto* global = builder.CreateGlobalString(bytes, name);
    llvm::Constant* zero = builder.getInt32(0);
    llvm::Constant* indices[] = {zero, zero};
    return llvm::ConstantExpr::getInBoundsGetElementPtr(
        global->getValueType(), global,
        llvm::ArrayRef<llvm::Constant*>(indices));
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
