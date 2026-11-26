#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/backend/LlvmCompat.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_MAJOR >= 21
#include "llvm/TargetParser/Triple.h"
#endif

namespace oreslang {
namespace wasm_actor_checked_v1 {

inline bool integer(const Expression* expr, std::int64_t n) {
    return expr && expr->kind == Expression::Kind::Integer && expr->integer == n;
}
inline bool variable(const Expression* expr, const std::string& name) {
    return expr && expr->kind == Expression::Kind::Variable && expr->name == name;
}
inline bool is_recursive_step(const Expression* expr,
                              const std::string& helper,
                              const std::string& parameter) {
    if (!expr || expr->kind != Expression::Kind::Call ||
        expr->name != helper || expr->arguments.size() != 1 ||
        expr->type_arguments_present || !expr->argument_names.empty()) return false;
    const Expression* arg = expr->arguments.front().get();
    return arg && arg->kind == Expression::Kind::Subtract &&
        variable(arg->left.get(), parameter) && integer(arg->right.get(), 1);
}

// This parser-certified first milestone is intentionally a closed expression
// machine. Do not silently interpret arbitrary Call, actor fields, early
// returns or source effects with the special emitter.
inline void require_source_shape(const Program& source) {
    auto deny = []() {
        throw std::runtime_error(
            "wasm32 checked recursive actors v1 require a single pure "
            "fnc f(int n): int { return n == 0 ? 0 : 1 + f(n - 1); }, "
            "one actor whose handler returns f(message), and constant main");
    };
    if (source.actors.size() != 1 || source.functions.size() != 2 ||
        !source.imports.empty() || !source.classes.empty() ||
        !source.generators.empty() || !source.cooperative_loops.empty() ||
        !source.contracts.empty() || !source.conforms.empty() ||
        !source.sum_types.empty() || !source.type_only_declarations.empty() ||
        !source.type_constraint_descriptors.empty()) deny();

    const Function* helper = nullptr;
    const Function* main = nullptr;
    for (const auto& fn : source.functions) {
        if (fn.name == "main") main = &fn;
        else if (helper) deny();
        else helper = &fn;
    }
    if (!helper || !main || helper->exported || helper->returns_void ||
        helper->return_type != ScalarType::I64 ||
        helper->parameters.size() != 1 ||
        helper->parameters[0].mode != Parameter::Mode::Value ||
        helper->parameters[0].scalar_type != ScalarType::I64 ||
        !helper->parameters[0].class_name.empty() ||
        !helper->parameters[0].generic_type.empty() ||
        !helper->generic_parameters.empty() ||
        !helper->locals.empty() || !helper->statements.empty() ||
        !helper->early_returns.empty() || !helper->actor_spawns.empty() ||
        !helper->actor_sends.empty() || !helper->actor_stops.empty() ||
        !helper->actor_field_writes.empty() || !helper->source_tasks.empty() ||
        !helper->future_pipelines.empty() || helper->async_source_i64 ||
        !helper->ready_await_actor.empty() || !helper->done_await_actor.empty() ||
        helper->caught_throw_value || helper->scalar_while ||
        !helper->result || !helper->method_owner.empty() ||
        !helper->c_export_u32_symbol.empty()) deny();
    if (main->parameters.size() || main->returns_void ||
        main->return_type != ScalarType::I64 ||
        !integer(main->result.get(), 42) ||
        !main->locals.empty() || !main->statements.empty() ||
        !main->early_returns.empty() || !main->actor_spawns.empty() ||
        !main->actor_sends.empty() || !main->actor_stops.empty() ||
        !main->actor_field_writes.empty() || !main->source_tasks.empty() ||
        !main->future_pipelines.empty() || main->async_source_i64 ||
        main->caught_throw_value || main->scalar_while ||
        !main->c_export_u32_symbol.empty()) deny();

    const auto& actor = source.actors.front();
    const auto& handler = actor.on_message;
    if (actor.max_actor_threads != MaxActorThreads::LOW ||
        actor.fields.size() != 1 ||
        !integer(actor.fields.front().initializer.get(), 0) ||
        handler.parameters.size() != 1 || handler.returns_void ||
        handler.return_type != ScalarType::I64 ||
        handler.parameters.front().mode != Parameter::Mode::Value ||
        !handler.parameters.front().class_name.empty() ||
        !handler.locals.empty() || !handler.statements.empty() ||
        !handler.early_returns.empty() ||
        !handler.actor_field_writes.empty() ||
        !handler.actor_sends.empty() || !handler.actor_stops.empty() ||
        !handler.actor_spawns.empty() || !handler.source_tasks.empty() ||
        !handler.future_pipelines.empty() || handler.async_source_i64 ||
        !handler.result || handler.result->kind != Expression::Kind::Call ||
        handler.result->name != helper->name ||
        handler.result->arguments.size() != 1 ||
        handler.result->type_arguments_present ||
        !handler.result->argument_names.empty() ||
        !variable(handler.result->arguments.front().get(),
                  handler.parameters.front().name)) deny();

    const std::string& param = helper->parameters.front().name;
    const Expression& root = *helper->result;
    if (root.kind != Expression::Kind::Conditional ||
        !root.condition || root.condition->kind != Expression::Kind::Equal ||
        !variable(root.condition->left.get(), param) ||
        !integer(root.condition->right.get(), 0) ||
        !integer(root.when_true.get(), 0) ||
        !root.when_false || root.when_false->kind != Expression::Kind::Add ||
        !(integer(root.when_false->left.get(), 1) &&
          is_recursive_step(root.when_false->right.get(), helper->name, param)) &&
        !(integer(root.when_false->right.get(), 1) &&
          is_recursive_step(root.when_false->left.get(), helper->name, param))) deny();
}

inline std::string emit(const Program& source) {
    require_source_shape(source);

    llvm::LLVMContext context;
    llvm::Module module("atto-wasm-actor-checked-trampoline-v1", context);
    const auto abi = TargetAbi::parse("wasm32-unknown-unknown");
    if (!abi.experimental_wasm32_ir())
        throw std::runtime_error("wasm32 checked trampoline ABI unavailable");
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(llvm::Triple(std::string(abi.triple())));
#else
    module.setTargetTriple(std::string(abi.triple()));
#endif
    module.setDataLayout("e-m:e-p:32:32-i64:64-n32:64-S128");
    const auto& layout = module.getDataLayout();
    if (layout.getPointerSizeInBits(0) != 32 || !layout.isLittleEndian())
        throw std::runtime_error("wasm32 checked trampoline target mismatch");

    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* zero = llvm::ConstantInt::get(i64, 0);
    auto* one = llvm::ConstantInt::get(i64, 1);
    auto* two = llvm::ConstantInt::get(i64, 2);
    auto* limit = llvm::ConstantInt::get(i64, 64);
    auto* status0 = llvm::ConstantInt::get(i32, 0);
    auto* status1 = llvm::ConstantInt::get(i32, 1);
    auto* trap = native_trap(module);
    auto* slotsType = llvm::ArrayType::get(i64, 64);
    auto global = [&](llvm::Type* type, llvm::Constant* initial,
                      const char* name) -> llvm::GlobalVariable* {
        return new llvm::GlobalVariable(
            module, type, false, llvm::GlobalValue::InternalLinkage,
            initial, name);
    };
    auto* lifecycle = global(i64, zero, "oreslang.wasm.actor.lifecycle");
    auto* depth = global(i64, zero, "oreslang.wasm.actor.frame.depth");
    auto* terminal = global(i32, status0, "oreslang.wasm.actor.frame.terminal");
    auto* slots = global(slotsType,
        llvm::ConstantAggregateZero::get(slotsType),
        "oreslang.wasm.actor.frame.slots");

    auto make = [&](const char* name, llvm::Type* result,
                    std::vector<llvm::Type*> parameters) -> llvm::Function* {
        return llvm::Function::Create(
            llvm::FunctionType::get(result, parameters, false),
            llvm::GlobalValue::ExternalLinkage, name, module);
    };
    auto* voidType = llvm::Type::getVoidTy(context);
    auto* initialize = make("oreslang_wasm_actor_init", voidType, {});
    auto* handler = make("oreslang_wasm_actor_on_message", i64, {i64});
    auto* teardown = make("oreslang_wasm_actor_teardown", voidType, {});
    auto* inspect = make("oreslang_wasm_actor_frame_status", i32, {});

    auto trap_if_invalid = [&](llvm::IRBuilder<>& b, llvm::Function* owner,
                               llvm::Value* valid) {
        auto* admitted = llvm::BasicBlock::Create(context, "admitted", owner);
        auto* rejected = llvm::BasicBlock::Create(context, "invalid", owner);
        b.CreateCondBr(valid, admitted, rejected);
        b.SetInsertPoint(rejected);
        b.CreateCall(trap);
        b.CreateUnreachable();
        b.SetInsertPoint(admitted);
    };

    llvm::IRBuilder<> begin(llvm::BasicBlock::Create(
        context, "entry", initialize));
    trap_if_invalid(begin, initialize,
        begin.CreateICmpEQ(begin.CreateLoad(i64, lifecycle), zero));
    begin.CreateStore(zero, depth);
    begin.CreateStore(status0, terminal);
    begin.CreateStore(one, lifecycle);
    begin.CreateRetVoid();

    // Explicit, compiler-owned two-phase push/unwind trampoline:
    // nested function calls are **not** emitted to the Wasm engine stack.
    // Each pushed source call occupies a private 8-byte guest frame. The
    // 65th push is denied before GEP, with all frames zeroed before trap.
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(
        context, "entry", handler));
    auto* stateOk = b.CreateICmpEQ(b.CreateLoad(i64, lifecycle), one);
    auto* statusOk = b.CreateICmpEQ(b.CreateLoad(i32, terminal), status0);
    auto* emptyStack = b.CreateICmpEQ(b.CreateLoad(i64, depth), zero);
    trap_if_invalid(b, handler,
        b.CreateAnd(stateOk, b.CreateAnd(statusOk, emptyStack)));
    // The restricted recurrence has a signed-i64 subtraction at every
    // nonzero level. Negative inputs (especially INT64_MIN) cannot be
    // lowered as unchecked wrapping arithmetic and then mislabeled as
    // a stack quota failure. Deny them before publishing a frame.
    trap_if_invalid(b, handler,
        b.CreateICmpSGE(&*handler->arg_begin(), zero));

    // The iterative machine needs no native-stack local allocations.
    // Transient scalar loop state is SSA; logical call data lives only in
    // the actor-owned frame slab.
    auto* initial = b.GetInsertBlock();

    auto* push = llvm::BasicBlock::Create(context, "atto.trampoline.push", handler);
    auto* enter = llvm::BasicBlock::Create(context, "atto.trampoline.enter", handler);
    auto* down = llvm::BasicBlock::Create(context, "atto.trampoline.down", handler);
    auto* unwind = llvm::BasicBlock::Create(context, "atto.trampoline.unwind", handler);
    auto* pop = llvm::BasicBlock::Create(context, "atto.trampoline.pop", handler);
    auto* done = llvm::BasicBlock::Create(context, "atto.trampoline.done", handler);
    auto* overflow = llvm::BasicBlock::Create(context, "atto.trampoline.overflow", handler);
    b.CreateBr(push);

    b.SetInsertPoint(push);
    auto* current = b.CreatePHI(i64, 2, "atto.trampoline.current");
    current->addIncoming(&*handler->arg_begin(), initial);
    auto* before = b.CreateLoad(i64, depth, "atto.frame.depth.before");
    b.CreateCondBr(b.CreateICmpULT(before, limit), enter, overflow);

    b.SetInsertPoint(enter);
    auto* slot = b.CreateInBoundsGEP(slotsType, slots, {zero, before},
        "atto.frame.private.slot");
    auto* value = current;
    b.CreateStore(value, slot)->setVolatile(true);
    b.CreateStore(b.CreateAdd(before, one), depth);
    b.CreateCondBr(b.CreateICmpEQ(value, zero), unwind, down);

    b.SetInsertPoint(down);
    // Only positive values reach this block, so signed subtraction is safe.
    auto* next = b.CreateSub(value, one, "atto.trampoline.next");
    current->addIncoming(next, down);
    b.CreateBr(push);

    b.SetInsertPoint(unwind);
    auto* answer = b.CreatePHI(i64, 2, "atto.trampoline.answer");
    answer->addIncoming(zero, enter);
    auto* count = b.CreateLoad(i64, depth, "atto.frame.unwind.depth");
    b.CreateCondBr(b.CreateICmpEQ(count, zero), done, pop);

    b.SetInsertPoint(pop);
    auto* index = b.CreateSub(count, one);
    auto* address = b.CreateInBoundsGEP(slotsType, slots, {zero, index});
    auto* parent = b.CreateLoad(i64, address, "atto.frame.unwind.value");
    parent->setVolatile(true);
    b.CreateStore(zero, address)->setVolatile(true);
    b.CreateStore(index, depth);
    auto* delta = b.CreateZExt(b.CreateICmpNE(parent, zero), i64);
    auto* accumulated = b.CreateAdd(answer, delta, "atto.trampoline.accumulated");
    answer->addIncoming(accumulated, pop);
    b.CreateBr(unwind);

    b.SetInsertPoint(done);
    b.CreateRet(answer);

    b.SetInsertPoint(overflow);
    // Never leave a stale guest frame after typed terminal exhaustion.
    // Volatile stores preserve cleanup even at -Oz.
    for (std::uint64_t i = 0; i < 64; ++i) {
        auto* address = b.CreateInBoundsGEP(slotsType, slots,
            {zero, llvm::ConstantInt::get(i64, i)});
        b.CreateStore(zero, address)->setVolatile(true);
    }
    // These stores are observable by the trusted host after the Wasm trap.
    // LLVM's optimizer must not erase them as dead stores merely because
    // the current guest activation ends with a noreturn llvm.trap.
    b.CreateStore(zero, depth)->setVolatile(true);
    b.CreateStore(status1, terminal)->setVolatile(true);
    b.CreateStore(two, lifecycle)->setVolatile(true);
    b.CreateCall(trap);
    b.CreateUnreachable();

    llvm::IRBuilder<> end(llvm::BasicBlock::Create(
        context, "entry", teardown));
    auto* canStop = end.CreateAnd(
        end.CreateICmpEQ(end.CreateLoad(i64, lifecycle), one),
        end.CreateAnd(
            end.CreateICmpEQ(end.CreateLoad(i64, depth), zero),
            end.CreateICmpEQ(end.CreateLoad(i32, terminal), status0)));
    trap_if_invalid(end, teardown, canStop);
    for (std::uint64_t i = 0; i < 64; ++i) {
        auto* address = end.CreateInBoundsGEP(slotsType, slots,
            {zero, llvm::ConstantInt::get(i64, i)});
        end.CreateStore(zero, address)->setVolatile(true);
    }
    end.CreateStore(two, lifecycle);
    end.CreateRetVoid();

    llvm::IRBuilder<> status(llvm::BasicBlock::Create(
        context, "entry", inspect));
    auto* observedStatus = status.CreateLoad(i32, terminal);
    observedStatus->setVolatile(true);
    status.CreateRet(observedStatus);

    std::string diagnostics;
    llvm::raw_string_ostream error_stream(diagnostics);
    if (llvm::verifyModule(module, &error_stream))
        throw std::runtime_error("wasm32 actor trampoline IR invalid: " +
                                 error_stream.str());
    std::string output;
    llvm::raw_string_ostream stream(output);
    module.print(stream, nullptr);
    return stream.str();
}
} // namespace wasm_actor_checked_v1
} // namespace oreslang
