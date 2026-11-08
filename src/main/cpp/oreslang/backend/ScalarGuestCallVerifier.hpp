#pragma once

#include <stdexcept>

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

namespace oreslang::backend_detail {

// This is intentionally stricter than a general interprocedural verifier.
// The current experimental scalar loop has NO guest-call lowering. Refuse
// unverified functions, direct/recursive guest calls, external FFI, indirect
// and inline-assembly calls, and exceptional call/invoke paths. Future
// language support must replace this with a proven interprocedural contract,
// never weaken it by guessing that a call target is metered.
inline void require_closed_scalar_guest_calls(const llvm::Module& module) {
    unsigned definitions = 0;
    for (const auto& fn : module) {
        if (fn.isDeclaration()) {
            if (fn.getIntrinsicID() != llvm::Intrinsic::trap)
                throw std::runtime_error("scalar guest module imports unverified foreign call");
            continue;
        }
        const llvm::StringRef name = fn.getName();
        if (name != "oreslang_rt_scalar_frame_init_v1" &&
            name != "oreslang_rt_scalar_frame_step_v1" &&
            name != "oreslang_rt_scalar_frame_cancel_v1" &&
            name != "oreslang_rt_scalar_frame_result_v1")
            throw std::runtime_error("scalar guest module contains unapproved guest function");
        ++definitions;
        for (const auto& block : fn) {
            for (const auto& instruction : block) {
                if (llvm::isa<llvm::IndirectBrInst>(&instruction))
                    throw std::runtime_error("scalar guest contains indirect CFG edge");
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call) continue;
                const auto* callee = call->getCalledFunction();
                if (!llvm::isa<llvm::CallInst>(call) ||
                    call->isInlineAsm() || callee == nullptr ||
                    callee->getIntrinsicID() != llvm::Intrinsic::trap ||
                    !call->arg_empty())
                    throw std::runtime_error("scalar guest contains unmetered or indirect call");
            }
        }
    }
    if (definitions != 4)
        throw std::runtime_error("scalar guest ABI must define exactly four v1 functions");

    // Names alone are not an ABI contract. A same-named function with the
    // wrong return type, argument count, variadic signature, linkage or
    // calling convention would make the C11 carrier invoke incompatible IR.
    // Preserve the exact expected four-function v1 ABI before emitting IR.
    auto& context = module.getContext();
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* voidTy = llvm::Type::getVoidTy(context);
    const llvm::Type* framePointer = nullptr;
    const auto check_abi = [&](const char* name, llvm::Type* returnType,
                               unsigned parameterCount) {
        const auto* fn = module.getFunction(name);
        if (!fn || fn->isDeclaration() ||
                fn->getLinkage() != llvm::GlobalValue::ExternalLinkage ||
                fn->getCallingConv() != llvm::CallingConv::C ||
                fn->getReturnType() != returnType ||
                fn->isVarArg() ||
                fn->arg_size() != parameterCount ||
                !fn->getFunctionType()->getParamType(0)->isPointerTy() ||
                (parameterCount == 2 &&
                 fn->getFunctionType()->getParamType(1) != i64))
            throw std::runtime_error("scalar guest has incompatible v1 frame ABI");
        if (!framePointer)
            framePointer = fn->getFunctionType()->getParamType(0);
        else if (framePointer != fn->getFunctionType()->getParamType(0))
            throw std::runtime_error("scalar guest frame pointer ABI differs across functions");
    };
    check_abi("oreslang_rt_scalar_frame_init_v1", voidTy, 2);
    check_abi("oreslang_rt_scalar_frame_step_v1", i32, 2);
    check_abi("oreslang_rt_scalar_frame_cancel_v1", voidTy, 1);
    check_abi("oreslang_rt_scalar_frame_result_v1", i64, 1);

    // A function-only whitelist must not silently allow a global, alias or
    // ifunc that introduces a second executable/import surface.
    if (module.global_begin() != module.global_end() ||
        module.alias_begin() != module.alias_end() ||
        module.ifunc_begin() != module.ifunc_end())
        throw std::runtime_error("scalar guest module contains unapproved global or alias");
}

} // namespace oreslang::backend_detail
