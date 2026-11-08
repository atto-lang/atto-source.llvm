#include "oreslang/backend/ScalarGuestCallVerifier.hpp"
#include "oreslang/backend/LlvmCompat.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

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

static llvm::Function* define_frame_method(
        llvm::Module& module, const char* name, llvm::Type* resultType,
        llvm::Type* framePointer, bool takesFuel) {
    auto& context = module.getContext();
    std::vector<llvm::Type*> parameters{framePointer};
    if (takesFuel) parameters.push_back(llvm::Type::getInt64Ty(context));
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(resultType, parameters, false),
        llvm::Function::ExternalLinkage, name, module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", fn));
    if (resultType->isVoidTy())
        builder.CreateRetVoid();
    else
        builder.CreateRet(llvm::Constant::getNullValue(resultType));
    return fn;
}

static llvm::Function* define_four(llvm::Module& module) {
    auto& context = module.getContext();
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* frameTy = llvm::StructType::getTypeByName(
        context, "oreslang.rt.scalar.frame.v1");
    if (!frameTy) {
        frameTy = llvm::StructType::create(
            context, {i64, i64, i64, i64},
            "oreslang.rt.scalar.frame.v1");
    }
    auto* framePtr = oreslang::native_pointer_type(context, frameTy);
    define_frame_method(module, "oreslang_rt_scalar_frame_init_v1",
                        llvm::Type::getVoidTy(context), framePtr, true);
    auto* step = define_frame_method(module, "oreslang_rt_scalar_frame_step_v1",
                                    llvm::Type::getInt32Ty(context), framePtr, true);
    define_frame_method(module, "oreslang_rt_scalar_frame_cancel_v1",
                        llvm::Type::getVoidTy(context), framePtr, false);
    define_frame_method(module, "oreslang_rt_scalar_frame_result_v1",
                        i64, framePtr, false);
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
        auto* trap = oreslang::native_trap(module);
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
        builder.CreateCall(step, {
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(
                step->getFunctionType()->getParamType(0))),
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1)});
        require(!admitted(module)); // even bounded-looking recursion needs metering
    }
    {
        llvm::Module module("indirect-call", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        auto* noargs = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
        auto* pointer = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(
                oreslang::native_pointer_type(context, noargs)));
        builder.CreateCall(noargs, pointer, {});
        require(!admitted(module));
    }
    {
        llvm::Module module("inline-assembly", context);
        auto* step = define_four(module);
        llvm::IRBuilder<> builder(step->getEntryBlock().getTerminator());
        auto* noargs = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
        auto* assembly = llvm::InlineAsm::get(noargs, "", "", true);
        builder.CreateCall(noargs, assembly, {});
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
#if LLVM_VERSION_MAJOR >= 20
        llvm::Intrinsic::getOrInsertDeclaration(&module, llvm::Intrinsic::donothing);
#else
        llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::donothing);
#endif
        require(!admitted(module));
    }
    // The ABI must be checked by argument types, arity, return type,
    // calling convention and linkage, not merely the allowed symbol names.
    {
        llvm::Module module("wrong-return", context);
        define_four(module);
        auto* result = module.getFunction("oreslang_rt_scalar_frame_result_v1");
        auto* framePointer = result->getFunctionType()->getParamType(0);
        result->eraseFromParent();
        define_frame_method(module, "oreslang_rt_scalar_frame_result_v1",
                            llvm::Type::getVoidTy(context), framePointer, false);
        require(!admitted(module));
    }
    {
        llvm::Module module("wrong-arity", context);
        auto* step = define_four(module);
        auto* framePointer = step->getFunctionType()->getParamType(0);
        step->eraseFromParent();
        define_frame_method(module, "oreslang_rt_scalar_frame_step_v1",
                            llvm::Type::getInt32Ty(context), framePointer, false);
        require(!admitted(module));
    }
    {
        llvm::Module module("wrong-pointer-parameter", context);
        auto* init = define_four(module);
        (void)init;
        auto* fn = module.getFunction("oreslang_rt_scalar_frame_init_v1");
        fn->eraseFromParent();
        define_frame_method(module, "oreslang_rt_scalar_frame_init_v1",
                            llvm::Type::getVoidTy(context),
                            llvm::Type::getInt64Ty(context), true);
        require(!admitted(module));
    }
    {
        llvm::Module module("variadic-abi", context);
        define_four(module);
        auto* fn = module.getFunction("oreslang_rt_scalar_frame_init_v1");
        auto* framePointer = fn->getFunctionType()->getParamType(0);
        fn->eraseFromParent();
        auto* varargs = llvm::Function::Create(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {framePointer, llvm::Type::getInt64Ty(context)}, true),
            llvm::Function::ExternalLinkage,
            "oreslang_rt_scalar_frame_init_v1", module);
        llvm::IRBuilder<> builder(
            llvm::BasicBlock::Create(context, "entry", varargs));
        builder.CreateRetVoid();
        require(!admitted(module));
    }
    {
        llvm::Module module("wrong-linkage", context);
        define_four(module);
        module.getFunction("oreslang_rt_scalar_frame_step_v1")
            ->setLinkage(llvm::GlobalValue::InternalLinkage);
        require(!admitted(module));
    }
    {
        llvm::Module module("wrong-calling-convention", context);
        define_four(module);
        module.getFunction("oreslang_rt_scalar_frame_step_v1")
            ->setCallingConv(llvm::CallingConv::Fast);
        require(!admitted(module));
    }
    {
        llvm::Module module("unapproved-global", context);
        define_four(module);
        new llvm::GlobalVariable(
            module, llvm::Type::getInt64Ty(context), false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1),
            "unauthorized_global");
        require(!admitted(module));
    }
    {
        // Wrong layout is rejected even when the named frame and four
        // function signatures otherwise look plausible.
        llvm::LLVMContext wrongContext;
        llvm::Module module("malformed-frame-layout", wrongContext);
        auto* i64 = llvm::Type::getInt64Ty(wrongContext);
        auto* i32 = llvm::Type::getInt32Ty(wrongContext);
        (void)llvm::StructType::create(
            wrongContext, {i64, i64, i32, i64},
            "oreslang.rt.scalar.frame.v1");
        define_four(module);
        require(!admitted(module));
    }
#if LLVM_VERSION_MAJOR < 15
    {
        llvm::LLVMContext wrongContext;
        llvm::Module module("wrong-typed-frame-pointer", wrongContext);
        auto* i64 = llvm::Type::getInt64Ty(wrongContext);
        (void)llvm::StructType::create(
            wrongContext, {i64, i64, i64, i64},
            "oreslang.rt.scalar.frame.v1");
        auto* wrongPtr = i64->getPointerTo();
        define_frame_method(module, "oreslang_rt_scalar_frame_init_v1",
                            llvm::Type::getVoidTy(wrongContext), wrongPtr, true);
        define_frame_method(module, "oreslang_rt_scalar_frame_step_v1",
                            llvm::Type::getInt32Ty(wrongContext), wrongPtr, true);
        define_frame_method(module, "oreslang_rt_scalar_frame_cancel_v1",
                            llvm::Type::getVoidTy(wrongContext), wrongPtr, false);
        define_frame_method(module, "oreslang_rt_scalar_frame_result_v1",
                            i64, wrongPtr, false);
        require(!admitted(module));
    }
#endif
    std::cout << "SCALAR_GUEST_CALL_ADMISSION_PASS\n";
}
