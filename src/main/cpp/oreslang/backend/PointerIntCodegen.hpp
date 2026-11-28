#pragma once
// LLVM MIR emitter: accepts only nominal Pointer<int> HIR whose full lifetime,
// initialized-element proofs and immutable scalar values were authorized by
// the standard OwnershipChecker stage. No local lexer/parser is linked here.
#include "oreslang/types/PointerIntHIR.hpp"
#include "oreslang/types/PointerIntOwnership.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang::pointer_int_aot {

// All source constructs and unsafe effects are validated before this emits IR.
// The optimizer may promote alloca into registers, but the emitted LLVM IR
// contains real typed stack memory, loads and stores, not an interpreter.
inline std::string emit_verified_ir(const Plan& plan) {
    if (!plan.ownership_admitted)
        throw std::runtime_error("Pointer<int> LLVM backend refuses unadmitted typed ownership HIR");
    // Defense in depth: an optimizer cannot use a stale ownership-admitted
    // bit after altering pointer indexes, initialization, effects or returns.
    verify_plan(plan);
    llvm::LLVMContext context;
    llvm::Module module("atto.pointer.int.aot.m1", context);
    llvm::IRBuilder<> builder(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* array = llvm::ArrayType::get(i64, plan.count);
    // Closed readonly helper takes a real stack-region address, not a wrapper.
    llvm::Function* readonly_helper = nullptr;
    if (!plan.readonly_helper.empty()) {
        auto* pointer_type = llvm::PointerType::getUnqual(context);
        auto* helper_type = llvm::FunctionType::get(i64, {pointer_type}, false);
        readonly_helper = llvm::Function::Create(helper_type, llvm::GlobalValue::InternalLinkage,
            "atto.pointer.readonly.helper", module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", readonly_helper));
        builder.CreateRet(builder.CreateLoad(i64, readonly_helper->getArg(0), "borrowed.read"));
    }
    llvm::Function* mutable_helper = nullptr;
    if (!plan.mutable_helper.empty()) {
        auto* pointer_type = llvm::PointerType::getUnqual(context);
        auto* helper_type = plan.mutable_scalar_parameter.empty() ?
            llvm::FunctionType::get(builder.getVoidTy(), {pointer_type}, false) :
            llvm::FunctionType::get(builder.getVoidTy(), {pointer_type, i64}, false);
        mutable_helper = llvm::Function::Create(helper_type, llvm::GlobalValue::InternalLinkage,
            "atto.pointer.mutable.helper", module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", mutable_helper));
        llvm::Value* value = plan.mutable_scalar_parameter.empty() ?
            static_cast<llvm::Value*>(builder.getInt64(plan.mutable_write)) : mutable_helper->getArg(1);
        builder.CreateStore(value, mutable_helper->getArg(0));
        builder.CreateRetVoid();
    }
    auto* main_type = llvm::FunctionType::get(i32, false);
    auto* main_fn = llvm::Function::Create(
        main_type, llvm::GlobalValue::ExternalLinkage, "main", module);
    auto* entry = llvm::BasicBlock::Create(context, "entry", main_fn);
    builder.SetInsertPoint(entry);
    auto* allocation = builder.CreateAlloca(array, nullptr, "pointer.int.region");
    if (plan.form == Plan::Form::BorrowedLocal) {
        // A scoped local-place read loan is real LLVM stack memory, with
        // authenticated authority erased only after typed ownership proof.
        builder.CreateStore(builder.getInt64(plan.local_initial),
            builder.CreateInBoundsGEP(array, allocation,
                {builder.getInt64(0), builder.getInt64(0)}, "pointer.local.initial"));
    }
    std::unordered_map<std::string, llvm::Value*> scalar_values;
    auto slot = [&](std::size_t index) -> llvm::Value* {
        return builder.CreateInBoundsGEP(array, allocation,
            {builder.getInt64(0), builder.getInt64(index)}, "pointer.int.slot");
    };
    if (plan.form == Plan::Form::ReusableSlot) {
        for (std::size_t i = 0; i < plan.slot_owners.size(); ++i) {
            const auto& [name, value] = plan.slot_owners[i];
            builder.CreateStore(builder.getInt64(value), slot(i));
            scalar_values.emplace(name, builder.getInt64(value));
        }
    }
    std::function<llvm::Value*(const Expr&)> lower;
    lower = [&](const Expr& expr) -> llvm::Value* {
        switch (expr.kind) {
        case Expr::Kind::Literal:
            return builder.getInt64(expr.known);
        case Expr::Kind::Variable:
            return scalar_values.at(expr.name);
        case Expr::Kind::Read:
            return builder.CreateLoad(i64, slot(expr.index), "pointer.read_at");
        case Expr::Kind::ReadonlyCall:
            return builder.CreateCall(readonly_helper, {slot(0)}, "borrowed.call");
        case Expr::Kind::Length:
            return builder.getInt64(plan.count);
        case Expr::Kind::SizeBytes:
            return builder.getInt64(plan.count * 8);
        case Expr::Kind::Neg:
            return builder.CreateNeg(lower(*expr.lhs), "scalar.negate");
        case Expr::Kind::Add:
            return builder.CreateAdd(lower(*expr.lhs), lower(*expr.rhs), "scalar.add");
        case Expr::Kind::Subtract:
            return builder.CreateSub(lower(*expr.lhs), lower(*expr.rhs), "scalar.subtract");
        case Expr::Kind::Multiply:
            return builder.CreateMul(lower(*expr.lhs), lower(*expr.rhs), "scalar.multiply");
        }
        throw std::runtime_error("unreachable pointer expression node");
    };
    for (const auto& step : plan.steps) {
        switch (step.kind) {
        case Step::Kind::Write:
            builder.CreateStore(lower(*step.value), slot(step.index));
            break;
        case Step::Kind::Fill: {
            auto* value = lower(*step.value);
            for (std::size_t i = 0; i < plan.count; ++i)
                builder.CreateStore(value, slot(i));
            break;
        }
        case Step::Kind::Swap: {
            auto* first = builder.CreateLoad(i64, slot(step.index), "pointer.swap.first");
            auto* second = builder.CreateLoad(i64, slot(step.second_index), "pointer.swap.second");
            builder.CreateStore(second, slot(step.index));
            builder.CreateStore(first, slot(step.second_index));
            break;
        }
        case Step::Kind::MutableCall:
            if (step.value) builder.CreateCall(mutable_helper, {slot(0), lower(*step.value)});
            else builder.CreateCall(mutable_helper, {slot(0)});
            break;
        case Step::Kind::Scalar:
            scalar_values.emplace(step.name, lower(*step.value));
            break;
        case Step::Kind::SlotBind:
        case Step::Kind::SlotDetach:
            // The independently validated local slot has no runtime identity.
            break;
        case Step::Kind::Close:
            if (plan.form == Plan::Form::BorrowedLocal) {
                // Non-owning unref revokes the handle, not the original local.
                break;
            }
            // Stack-promotion drop: overwrite every slot using volatile stores
            // before return. The stack frame is then reclaimed by the ABI.
            // No pointer or address can escape this function.
            for (std::size_t i = 0; i < plan.count; ++i) {
                auto* wiped = builder.CreateStore(builder.getInt64(0), slot(i));
                wiped->setVolatile(true);
            }
            break;
        }
    }
    builder.CreateRet(builder.CreateTrunc(lower(*plan.result), i32, "native.exit"));
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    if (llvm::verifyModule(module, &diagnostics))
        throw std::runtime_error("Pointer<int> LLVM verification failure: " + diagnostics.str());
    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}


} // namespace oreslang::pointer_int_aot
