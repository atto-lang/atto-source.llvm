#pragma once

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <cstdint>
#include <stdexcept>

namespace oreslang::backend {

// Emission boundary for an already typed, actor-owned logical Ref<T>.
// The parser's existing i64 and Array<int> class fields are NOT Ref<T>,
// and must never be passed to this lowering.
// 
// The trusted host/supervisor supplies the actor arena's current base, span,
// and ORIGINAL full-generation proof entry. The validator is a privileged
// runtime callback bound to that proof entry, not a guest-provided boolean.
// It must authenticate heap identity, incarnation, original generation,
// live allocation, and the exact encoded offset. A new proof reconstructed
// from the offset is NOT valid: it would permit ABA after reuse.
//
// The returned pointer is transient and MUST NOT survive await, a carrier
// change, actor retirement, or an FFI boundary. A caller must lower reads/
// writes immediately while holding the required borrow/actor turn.
struct ActorRef32Access final {
    llvm::Value* field_address = nullptr;        // authenticated object i32 slot
    llvm::Value* actor_arena_base = nullptr;     // admitted actor heap base
    llvm::Value* actor_arena_span = nullptr;     // native i64 byte span
    llvm::Value* original_proof = nullptr;       // trusted opaque metadata pointer
    llvm::FunctionCallee proof_check;             // i1 (ptr proof, ptr arena, i64 span, i32 stored)
};

inline llvm::Value* emit_checked_actor_ref32_address(
        llvm::Module& module, llvm::IRBuilder<>& builder,
        const ActorRef32Access& access, llvm::Type* object_type,
        std::uint64_t access_bytes) {
    // Initial compiler slice only handles i64 object payloads. Never let a
    // caller understate the actual load/store width when validating bounds.
    // Other types need an independently verified DataLayout-sized lowering.
    if (!access.field_address || !access.actor_arena_base ||
        !access.actor_arena_span || !access.original_proof ||
        !object_type || !object_type->isIntegerTy(64) || access_bytes != 8)
        throw std::invalid_argument("missing original actor reference proof or invalid access");

    llvm::LLVMContext& context = module.getContext();
    auto* i1 = llvm::Type::getInt1Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* ptr = llvm::Type::getInt8Ty(context)->getPointerTo();
    if (!access.field_address->getType()->isPointerTy() ||
        !access.actor_arena_base->getType()->isPointerTy() ||
        !access.original_proof->getType()->isPointerTy() ||
        access.actor_arena_span->getType() != i64 ||
        object_type->isVoidTy() || !object_type->isSized())
        throw std::invalid_argument("invalid actor reference LLVM operand types");

    // FunctionCallee accessors are non-const, so read them through a local copy.
    llvm::FunctionCallee proof_check = access.proof_check;
    auto* check_ty = proof_check.getFunctionType();
    if (!check_ty || check_ty->isVarArg() ||
        check_ty->getNumParams() != 4 ||
        check_ty->getReturnType() != i1 ||
        !check_ty->getParamType(0)->isPointerTy() ||
        !check_ty->getParamType(1)->isPointerTy() ||
        check_ty->getParamType(2) != i64 ||
        check_ty->getParamType(3) != i32)
        throw std::invalid_argument("actor reference proof validator ABI mismatch");

    // The ref32 word is identical on 32- and 64-bit targets. What differs is
    // the native address space the decoded pointer must fit in. Never guess
    // it: a layout-less module, an exotic width, or a target whose GEP index
    // width differs from its pointer width (fat/capability pointers) fails.
    if (module.getDataLayoutStr().empty())
        throw std::invalid_argument("actor-local ref32 requires a proven target ABI");
    const llvm::DataLayout& layout = module.getDataLayout();
    const unsigned pointer_bits = layout.getPointerSizeInBits(0);
    if ((pointer_bits != 64 && pointer_bits != 32) ||
        layout.getIndexSizeInBits(0) != pointer_bits)
        throw std::invalid_argument(
            "actor-local ref32 requires a 32- or 64-bit flat-pointer target ABI");
    const bool native32 = pointer_bits == 32;
    auto* intptr = llvm::Type::getIntNTy(context, pointer_bits);
    // Highest representable native address. On 32-bit targets this is far
    // below the 32-GiB ref32 span, so it becomes the binding bound.
    const std::uint64_t address_max = native32 ? UINT64_C(0xFFFFFFFF) : UINT64_MAX;

    auto* active = builder.GetInsertBlock();
    if (!active || active->getTerminator() || !active->getParent() ||
        active->getParent()->getParent() != &module)
        throw std::invalid_argument("actor reference must lower inside an active function");

    auto* fn = active->getParent();
    auto* source_bounds_block = llvm::BasicBlock::Create(
        context, "ref32.source.bounds", fn);
    auto* source_read_block = llvm::BasicBlock::Create(
        context, "ref32.source.read", fn);
    auto* proof_block = llvm::BasicBlock::Create(context, "ref32.proof", fn);
    auto* bounds_block = llvm::BasicBlock::Create(context, "ref32.bounds", fn);
    auto* safe_block = llvm::BasicBlock::Create(context, "ref32.deref", fn);
    auto* trap_block = llvm::BasicBlock::Create(context, "ref32.invalid", fn);

    // Even the SOURCE reference field may be an untrusted native address.
    // Validate its four-byte read against this arena BEFORE loading any
    // encoded bits. Target-proof validation only occurs after that read, so
    // it cannot protect the source-field address by itself.
    builder.CreateBr(source_bounds_block);
    builder.SetInsertPoint(source_bounds_block);
    llvm::Value* base = builder.CreatePtrToInt(
        access.actor_arena_base, intptr, "ref32.base.address");
    llvm::Value* source = builder.CreatePtrToInt(
        access.field_address, intptr, "ref32.source.address");
    if (native32) {
        base = builder.CreateZExt(base, i64, "ref32.base.wide");
        source = builder.CreateZExt(source, i64, "ref32.source.wide");
    }
    const std::uint64_t max_span = native32
        ? address_max : UINT64_C(34359738360);
    auto* span_compatible = builder.CreateICmpULE(access.actor_arena_span,
        llvm::ConstantInt::get(i64, max_span));
    auto* source_span_enough = builder.CreateICmpUGE(
        access.actor_arena_span, llvm::ConstantInt::get(i64, 4));
    auto* source_after_base = builder.CreateICmpUGE(source, base);
    auto* source_delta = builder.CreateSub(source, base, "ref32.source.delta");
    auto* source_fits = builder.CreateICmpULE(source_delta,
        builder.CreateSub(access.actor_arena_span, llvm::ConstantInt::get(i64, 4)),
        "ref32.source.fits");
    auto* pointer_max = llvm::ConstantInt::get(i64, address_max);
    auto* arena_no_wrap = builder.CreateICmpULE(
        builder.CreateSub(access.actor_arena_span, llvm::ConstantInt::get(i64, 1)),
        builder.CreateSub(pointer_max, base), "ref32.arena.no_wrap");
    auto* base_nonzero = builder.CreateICmpNE(
        base, llvm::ConstantInt::get(i64, 0));
    auto* base_aligned = builder.CreateICmpEQ(
        builder.CreateAnd(base, llvm::ConstantInt::get(i64, 7)),
        llvm::ConstantInt::get(i64, 0));
    auto* source_aligned = builder.CreateICmpEQ(
        builder.CreateAnd(source, llvm::ConstantInt::get(i64, 3)),
        llvm::ConstantInt::get(i64, 0));
    auto* source_valid = builder.CreateAnd(
        builder.CreateAnd(source_span_enough, span_compatible),
        builder.CreateAnd(source_after_base,
            builder.CreateAnd(source_fits,
                builder.CreateAnd(arena_no_wrap,
                    builder.CreateAnd(base_nonzero,
                        builder.CreateAnd(base_aligned, source_aligned))))),
        "ref32.source.valid");
    builder.CreateCondBr(source_valid, source_read_block, trap_block);

    builder.SetInsertPoint(source_read_block);
    // Physical guest storage is *always* i32. Do not load a machine pointer
    // or reinterpret the i32 field as a C ABI pointer.
    auto* field_ptr = builder.CreatePointerCast(access.field_address,
        i32->getPointerTo(), "ref32.field.i32.ptr");
    auto* encoded = builder.CreateLoad(i32, field_ptr, "ref32.encoded");
    auto* not_null = builder.CreateICmpNE(
        encoded, llvm::ConstantInt::get(i32, 0), "ref32.nonnull");
    builder.CreateCondBr(not_null, proof_block, trap_block);

    builder.SetInsertPoint(proof_block);
    auto* auth_proof = builder.CreatePointerCast(
        access.original_proof, check_ty->getParamType(0));
    auto* auth_base = builder.CreatePointerCast(
        access.actor_arena_base, check_ty->getParamType(1));
    auto* authorized = builder.CreateCall(proof_check,
        {auth_proof, auth_base, access.actor_arena_span, encoded},
        "ref32.authenticated");
    builder.CreateCondBr(authorized, bounds_block, trap_block);

    builder.SetInsertPoint(bounds_block);
    // Existing compressed encoding: (offset >> 3) + 1, zero means null.
    // Zero extend BEFORE subtracting, so UINT32_MAX is handled correctly.
    auto* wide = builder.CreateZExt(encoded, i64, "ref32.wide");
    auto* index = builder.CreateSub(wide, llvm::ConstantInt::get(i64, 1),
                                   "ref32.zero_based");
    auto* offset = builder.CreateShl(index, llvm::ConstantInt::get(i64, 3),
                                    "ref32.byte_offset");

    auto* width = llvm::ConstantInt::get(i64, access_bytes);
    auto* span_enough = builder.CreateICmpUGE(access.actor_arena_span, width);
    // An actor arena exceeding the compressed encoding's addressable span
    // must have been assigned native64 by the physical-layout planner.
    // On 32-bit targets the span is additionally capped by the address space.
    auto* max_offset = builder.CreateSub(access.actor_arena_span, width);
    auto* inside = builder.CreateICmpULE(offset, max_offset);

    // LLVM GEP does not itself guarantee that converting a byte offset to
    // a pointer stays within the host pointer range. Reject wrap explicitly.
    // All checks run in i64 on both widths; a 32-bit base is zero-extended.
    auto* access_end = builder.CreateAdd(offset,
        llvm::ConstantInt::get(i64, access_bytes - 1), "ref32.access_end");
    auto* ceiling = llvm::ConstantInt::get(i64, address_max);
    // Checked separately so that `ceiling - access_end` below can never be
    // an underflowed (and therefore permissive) bound on a 32-bit target.
    auto* end_representable = builder.CreateICmpULE(access_end, ceiling,
                                                    "ref32.end.representable");
    auto* no_wrap = builder.CreateICmpULE(base,
        builder.CreateSub(ceiling, access_end));
    auto* admitted = builder.CreateAnd(
        builder.CreateAnd(builder.CreateAnd(span_enough, span_compatible), inside),
        builder.CreateAnd(base_nonzero,
            builder.CreateAnd(base_aligned,
                builder.CreateAnd(end_representable, no_wrap))));
    builder.CreateCondBr(admitted, safe_block, trap_block);

    builder.SetInsertPoint(trap_block);
    builder.CreateCall(llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::trap));
    builder.CreateUnreachable();

    builder.SetInsertPoint(safe_block);
    // Deliberately NOT an inbounds GEP: if the trusted arena descriptor is
    // invalid, a poison value must not become an optimizer shortcut.
    // The offset is proven <= address_max above, so truncating it to the
    // 32-bit native index width is lossless; never hand GEP a wider index.
    llvm::Value* native_offset = native32
        ? builder.CreateTrunc(offset, intptr, "ref32.byte_offset.native")
        : offset;
    auto* bytes_ptr = builder.CreatePointerCast(access.actor_arena_base, ptr);
    auto* raw = builder.CreateGEP(llvm::Type::getInt8Ty(context),
                                  bytes_ptr, native_offset, "ref32.native.address");
    return builder.CreatePointerCast(raw, object_type->getPointerTo(),
                                     "ref32.checked.address");
}

inline llvm::Value* emit_checked_actor_ref32_load(
        llvm::Module& module, llvm::IRBuilder<>& builder,
        const ActorRef32Access& access, llvm::Type* type,
        std::uint64_t bytes) {
    auto* ptr = emit_checked_actor_ref32_address(
        module, builder, access, type, bytes);
    return builder.CreateLoad(type, ptr, "ref32.value");
}

inline void emit_checked_actor_ref32_store(
        llvm::Module& module, llvm::IRBuilder<>& builder,
        const ActorRef32Access& access, llvm::Value* value,
        std::uint64_t bytes) {
    if (!value) throw std::invalid_argument("missing actor reference value");
    auto* ptr = emit_checked_actor_ref32_address(
        module, builder, access, value->getType(), bytes);
    builder.CreateStore(value, ptr);
}

} // namespace oreslang::backend
