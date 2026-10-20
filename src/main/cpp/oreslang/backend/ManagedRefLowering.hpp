#pragma once

#include <cstdint>
#include <stdexcept>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace oreslang::backend {

/**
 * Lower one *already ownership-checked* actor-relative managed reference.
 * The physical guest field is i32, even on LP64.
 *
 * This is an LLVM backend building block, NOT a source-level Ref<T> feature.
 * A compiler-owned actor-turn admission must provide region_base and a
 * runtime-backed liveness/provenance check: never pass a guest-controlled i1
 * or a raw pointer taken from an untrusted message as "trusted_live".
 *
 * A stale 32-bit offset is never distinguishable after slot recycling:
 * the current ActorArena compact API quarantines published slots until the
 * owning actor's retirement, and the compiler must preserve that policy.
 *
 * Reject block MUST be a terminal fail-closed path and it remains the
 * caller's responsibility to wire actor epoch/loan checks to trusted_live.
 */
inline llvm::Value* lower_checked_actor_compressed32_address(
        llvm::IRBuilder<>& builder,
        llvm::Value* region_base,
        llvm::Value* region_bytes,
        llvm::Value* stored_ref32,
        llvm::Value* trusted_live,
        std::uint64_t access_bytes,
        llvm::BasicBlock* reject) {
    llvm::BasicBlock* current = builder.GetInsertBlock();
    if (!current || !current->getParent() || !reject ||
            reject->getParent() != current->getParent() ||
            reject == current || access_bytes == 0)
        throw std::invalid_argument("compressed32 lowering needs a valid checked CFG");
    auto* module = current->getModule();
    if (!module || module->getDataLayout().getPointerSizeInBits(0) != 64)
        throw std::invalid_argument("compressed32 lowering requires a native64 data layout");

    auto& context = builder.getContext();
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i1 = llvm::Type::getInt1Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    if (!region_base || !region_base->getType()->isPointerTy() ||
            region_base->getType()->getPointerAddressSpace() != 0 ||
            !region_bytes || region_bytes->getType() != i64 ||
            !stored_ref32 || stored_ref32->getType() != i32 ||
            !trusted_live || trusted_live->getType() != i1)
        throw std::invalid_argument("compressed32 lowering received incorrect LLVM types");

    auto* function = current->getParent();
    // Null must branch away before any attempted dereference. Do not emit
    // 'inbounds' or add invalid LLVM alias/noalias assumptions for unchecked
    // guest-origin memory.
    llvm::Value* nonnull = builder.CreateICmpNE(
        stored_ref32, llvm::ConstantInt::get(i32, 0), "actor.ref.nonnull");
    llvm::Value* base_nonnull = builder.CreateIsNotNull(region_base, "actor.arena.present");
    llvm::Value* as_i64 = builder.CreateZExt(stored_ref32, i64, "actor.ref.zext");
    llvm::Value* index = builder.CreateSub(as_i64,
        llvm::ConstantInt::get(i64, 1), "actor.ref.index");
    llvm::Value* offset = builder.CreateShl(index,
        llvm::ConstantInt::get(i64, 3), "actor.ref.byte.offset");
    llvm::Value* start_in_bounds = builder.CreateICmpULT(
        offset, region_bytes, "actor.ref.start.in.bounds");
    llvm::Value* remaining = builder.CreateSub(
        region_bytes, offset, "actor.ref.remaining");
    llvm::Value* whole_access_in_bounds = builder.CreateICmpULE(
        llvm::ConstantInt::get(i64, access_bytes), remaining, "actor.ref.access.in.bounds");
    llvm::Value* valid = builder.CreateAnd(nonnull, base_nonnull, "actor.ref.nonnull.base");
    valid = builder.CreateAnd(valid, trusted_live, "actor.ref.actor.provenance");
    valid = builder.CreateAnd(valid, start_in_bounds, "actor.ref.start.valid");
    valid = builder.CreateAnd(valid, whole_access_in_bounds, "actor.ref.access.valid");
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(
        context, "actor.ref.checked", function);
    builder.CreateCondBr(valid, ok, reject);
    builder.SetInsertPoint(ok);
    // This is an ordinary GEP, deliberately NOT an inbounds GEP: the trusted
    // native arena allocator guarantees a real mapped base+length range, but
    // this lowering primitive does not on its own prove allocation provenance.
    return builder.CreateGEP(i8, region_base, offset, "actor.ref.address");
}

} // namespace oreslang::backend
