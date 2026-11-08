#include "oreslang/backend/ScalarGuestCallVerifier.hpp"

#include <iostream>
#include <stdexcept>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

static void require(bool value) {
    if (!value) throw std::runtime_error("scalar guest call admission regression");
}

static llvm::Function* define_noop(llvm::Module& module, const char* name) {
    auto& context = module.getContext();
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
        llvm::Function::ExternalLinkage, name, module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", fn));
    builder.CreateRetVoid();
    return fn;
}

static llvm::Function* define_four(llvm::Module& module) {
    define_noop(module, "oreslang_rt_scalar_frame_init_v1");
    auto* step = define_noop(module, "oreslang_rt_scalar_frame_step_v1");
    define_noop(module, "oreslang_rt_scalar_frame_cancel_v1");
    define_noop(module, "oreslang_rt_scalar_frame_result_v1");
    return step;
}

static bool admitted(const llvm::Module& module) {
    try {
        oreslang::backend_detail::require_closed_scalar_guest_calls(module);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

int main() {
    llvm::LLVMContext context;
    {
        llvm::Module module("valid-trap-only", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        auto* trap = llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::trap);
        builder.CreateCall(trap);
        require(admitted(module)); // legitimate fail-closed trap is the only guest call
    }
    {
        llvm::Module module("foreign-import", context);
        define_four(module);
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
            llvm::Function::ExternalLinkage, "foreign_unverified", module);
        require(!admitted(module));
    }
    {
        llvm::Module module("direct-recursion", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        builder.CreateCall(step);
        require(!admitted(module)); // even bounded-looking recursion needs metering
    }
    {
        llvm::Module module("indirect-call", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        auto* pointer = llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0));
        builder.CreateCall(step->getFunctionType(), pointer, {});
        require(!admitted(module));
    }
    {
        llvm::Module module("inline-assembly", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        auto* assembly = llvm::InlineAsm::get(step->getFunctionType(), "", "", true);
        builder.CreateCall(step->getFunctionType(), assembly, {});
        require(!admitted(module));
    }
    {
        llvm::Module module("extra-guest-function", context);
        define_four(module);
        define_noop(module, "unverified_helper");
        require(!admitted(module));
    }
    {
        llvm::Module module("missing-required-function", context);
        define_noop(module, "oreslang_rt_scalar_frame_step_v1");
        require(!admitted(module));
    }
    {
        llvm::Module module("unexpected-intrinsic", context);
        define_four(module);
        llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::donothing);
        require(!admitted(module));
    }
    std::cout << "SCALAR_GUEST_CALL_ADMISSION_PASS\n";
}
