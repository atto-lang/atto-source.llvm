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
}

} // namespace oreslang::backend_detail
