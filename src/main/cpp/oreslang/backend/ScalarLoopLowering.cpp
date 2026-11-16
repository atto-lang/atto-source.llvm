#include "oreslang/backend/LlvmCodegen.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "oreslang/backend/LlvmCompat.hpp"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {

std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop) {
    if (loop.initial < 0 || loop.upper_bound < loop.initial ||
            loop.upper_bound > 1000000000LL)
        throw std::runtime_error("invalid experimental scalar loop bounds");

    // ABI v0: caller allocates an exclusively owned, pinned, zero-initialized
    // 24-byte frame. [state, counter, magic] are never stack slots across a
    // return-to-scheduler boundary. A real actor carrier must serially own
    // all calls; this compiler fragment does NOT implement actor dispatch.
    llvm::LLVMContext context;
    llvm::Module module("oreslang-native-scalar-loop", context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* voidTy = llvm::Type::getVoidTy(context);
    auto* ptr = native_pointer_type(context, i64);
    auto* frameTy = llvm::StructType::create(context, {i64, i64, i64},
                                             "oreslang.rt.scalar.frame");
    constexpr std::uint64_t kMagic = 0x4f5245534c4f4f50ULL;
    const auto constant = [&](std::uint64_t n) { return llvm::ConstantInt::get(i64, n); };
    const auto field = [&](llvm::IRBuilder<>& b, llvm::Value* frame, unsigned index) {
        return b.CreateStructGEP(frameTy, frame, index, "frame.field");
    };
    const auto fail = [&](llvm::IRBuilder<>& b) {
        b.CreateCall(native_trap(module));
        b.CreateUnreachable();
    };
    const auto entryGuard = [&](llvm::Function* fn, llvm::Value* frame, llvm::IRBuilder<>& b) {
        auto* ok = llvm::BasicBlock::Create(context, "frame.nonnull", fn);
        auto* bad = llvm::BasicBlock::Create(context, "frame.null", fn);
        b.CreateCondBr(b.CreateICmpNE(frame, llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptr))), ok, bad);
        b.SetInsertPoint(bad);
        fail(b);
        b.SetInsertPoint(ok);
    };
    // Frame-integrity predicates shared by step/cancel/result. A durable
    // heap continuation may only ever hold a counter inside the compiler-
    // proven [initial, upperBound] interval, and the terminal DONE state is
    // only reachable by committing exactly upperBound. Anything else is a
    // corrupted, recycled or forged frame and must trap, not be published.
    auto* initialBound = llvm::ConstantInt::getSigned(i64, loop.initial);
    auto* terminalBound = llvm::ConstantInt::getSigned(i64, loop.upper_bound);
    const auto counterInRange = [&](llvm::IRBuilder<>& b, llvm::Value* counter) {
        return b.CreateAnd(b.CreateICmpSGE(counter, initialBound),
                           b.CreateICmpSLE(counter, terminalBound), "frame.counter.in.range");
    };
    const auto doneIsExact = [&](llvm::IRBuilder<>& b, llvm::Value* state,
                                 llvm::Value* counter) {
        return b.CreateOr(b.CreateICmpNE(state, constant(2)),
                          b.CreateICmpEQ(counter, terminalBound), "frame.done.exact");
    };

    auto* init = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_init", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", init));
        auto* frame = &*init->arg_begin();
        entryGuard(init, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* fresh = llvm::BasicBlock::Create(context, "fresh", init);
        auto* invalid = llvm::BasicBlock::Create(context, "already.initialized", init);
        // state == 0 alone is not proof of a fresh frame: accepting dirty
        // counter/magic words could resurrect a recycled incarnation. The
        // ABI requires a fresh, fully zero-initialized, exclusively owned frame.
        auto* freshState = b.CreateICmpEQ(b.CreateLoad(i64, stateAddress), constant(0));
        auto* freshCounter = b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 1)), constant(0));
        auto* freshMagic = b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(0));
        b.CreateCondBr(b.CreateAnd(freshState, b.CreateAnd(freshCounter, freshMagic),
                                   "frame.all.zero"),
                       fresh, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(fresh);
        b.CreateStore(constant(1), stateAddress);
        b.CreateStore(constant(static_cast<std::uint64_t>(loop.initial)), field(b, frame, 1));
        b.CreateStore(constant(kMagic), field(b, frame, 2));
        b.CreateRetVoid();
    }

    // Status: 0 DONE, 1 COOPERATE, 2 CANCELLED. Zero fuel is a harmless
    // handoff without guest side effects. Fuel >512 traps, not an exemption
    // from the carrier reduction cap. No native stack survives a return.
    auto* step = llvm::Function::Create(
        llvm::FunctionType::get(i32, {ptr, i64}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_step", module);
    {
        auto arg = step->arg_begin();
        llvm::Value* frame = &*arg++;
        llvm::Value* fuel = &*arg;
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", step));
        entryGuard(step, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* counterAddress = field(b, frame, 1);
        auto* valid = llvm::BasicBlock::Create(context, "frame.valid", step);
        auto* invalid = llvm::BasicBlock::Create(context, "frame.invalid", step);
        llvm::Value* magic = b.CreateLoad(i64, field(b, frame, 2));
        llvm::Value* cap = b.CreateICmpULE(fuel, constant(512));
        b.CreateCondBr(b.CreateAnd(b.CreateICmpEQ(magic, constant(kMagic)), cap),
                       valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);

        auto* active = llvm::BasicBlock::Create(context, "active", step);
        auto* done = llvm::BasicBlock::Create(context, "done", step);
        auto* cancelled = llvm::BasicBlock::Create(context, "cancelled", step);
        auto* loopHead = llvm::BasicBlock::Create(context, "loop.check", step);
        auto* advance = llvm::BasicBlock::Create(context, "loop.commit", step);
        auto* batchExit = llvm::BasicBlock::Create(context, "loop.batch.exit", step);
        auto* exhausted = llvm::BasicBlock::Create(context, "cooperate", step);
        auto* complete = llvm::BasicBlock::Create(context, "loop.complete", step);
        auto* state = b.CreateLoad(i64, stateAddress, "state");
        // This counter is part of a long-lived heap continuation. Reject
        // corruption/stale frame replay in EVERY state, once per dispatch and
        // before any guest reduction: otherwise i64 wrapping could bypass the
        // fuel cap, and a cancelled/done frame could carry a bogus counter.
        auto* first = b.CreateLoad(i64, counterAddress, "counter.at.dispatch");
        auto* counterValid = llvm::BasicBlock::Create(context, "counter.valid", step);
        b.CreateCondBr(counterInRange(b, first), counterValid, invalid);
        b.SetInsertPoint(counterValid);
        // The scheduler may consume DONE (status 0) without ever calling
        // result(). A forged terminal state must therefore trap here, in
        // step(), unless the committed counter is exactly the terminal value.
        auto* stateConsistent = llvm::BasicBlock::Create(
            context, "frame.state.consistent", step);
        b.CreateCondBr(doneIsExact(b, state, first), stateConsistent, invalid);
        b.SetInsertPoint(stateConsistent);
        auto* sw = b.CreateSwitch(state, invalid, 3);
        sw->addCase(constant(1), active);
        sw->addCase(constant(2), done);
        sw->addCase(constant(3), cancelled);

        b.SetInsertPoint(done);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(cancelled);
        b.CreateRet(llvm::ConstantInt::get(i32, 2));
        b.SetInsertPoint(active);
        // Calculate the exclusive endpoint once per dispatch. Both the
        // source bound and admitted fuel fit safely in signed i64 here.
        // Loop backedges have NO repeated fuel test, time read or scheduler
        // callback. The only loop condition is the batch endpoint. `first`
        // was range-checked above, so first + fuel (<= 1e9 + 512) cannot wrap.
        auto* upper = terminalBound;
        auto* byFuel = b.CreateAdd(first, fuel, "dispatch.end.by.fuel");
        auto* endByFuel = b.CreateICmpULT(byFuel, upper);
        auto* stop = b.CreateSelect(endByFuel, byFuel, upper, "dispatch.batch.stop");
        b.CreateBr(loopHead);
        b.SetInsertPoint(loopHead);
        // The induction value lives in SSA, seeded from the validated
        // dispatch counter, so the batch length is bounded by
        // (stop - first) structurally rather than by re-reading the heap
        // frame on every backedge. Every tick is still committed to the frame.
        auto* current = b.CreatePHI(i64, 2, "counter.before");
        current->addIncoming(first, active);
        b.CreateCondBr(b.CreateICmpSLT(current, stop), advance, batchExit);

        b.SetInsertPoint(batchExit);
        // The existing frame protocol returns DONE (0) before COOPERATE (1)
        // when a dispatch ends at exactly the source bound, including fuel=0.
        b.CreateCondBr(b.CreateICmpSLT(current, upper), exhausted, complete);
        b.SetInsertPoint(complete);
        b.CreateStore(constant(2), stateAddress);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(exhausted);
        b.CreateRet(llvm::ConstantInt::get(i32, 1));
        b.SetInsertPoint(advance);
        // Source body has exactly one nonthrowing, bounded +1 effect.
        // Commit the frame counter before a later dispatch can cooperate.
        auto* next = b.CreateNSWAdd(current, constant(1), "counter.committed");
        b.CreateStore(next, counterAddress);
        b.CreateBr(loopHead);
        current->addIncoming(next, advance);
    }

    auto* cancel = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_cancel", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", cancel));
        auto* frame = &*cancel->arg_begin();
        entryGuard(cancel, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* s = b.CreateLoad(i64, stateAddress);
        auto* valid = llvm::BasicBlock::Create(context, "valid", cancel);
        auto* invalid = llvm::BasicBlock::Create(context, "invalid", cancel);
        auto* known = b.CreateAnd(b.CreateICmpUGE(s, constant(1)),
                                   b.CreateICmpULE(s, constant(3)));
        // Cancellation is an idempotent no-op on a terminal frame, so it must
        // not launder a corrupt counter or a forged DONE into "retired".
        auto* counter = b.CreateLoad(i64, field(b, frame, 1), "cancel.counter");
        auto* integrity = b.CreateAnd(counterInRange(b, counter), doneIsExact(b, s, counter));
        b.CreateCondBr(b.CreateAnd(known, b.CreateAnd(integrity,
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic)))),
            valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        auto* active = llvm::BasicBlock::Create(context, "cancel.active", cancel);
        auto* final = llvm::BasicBlock::Create(context, "cancel.final", cancel);
        b.CreateCondBr(b.CreateICmpEQ(s, constant(1)), active, final);
        b.SetInsertPoint(active);
        b.CreateStore(constant(3), stateAddress);
        b.CreateBr(final);
        b.SetInsertPoint(final);
        b.CreateRetVoid();
    }

    auto* result = llvm::Function::Create(
        llvm::FunctionType::get(i64, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_result", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", result));
        auto* frame = &*result->arg_begin();
        entryGuard(result, frame, b);
        auto* valid = llvm::BasicBlock::Create(context, "result.ready", result);
        auto* invalid = llvm::BasicBlock::Create(context, "result.invalid", result);
        // DONE alone does not authorize a result: a forged terminal state with
        // an incomplete (or overshot) counter must trap instead of publishing
        // a silently truncated scalar guest result.
        auto* committed = b.CreateLoad(i64, field(b, frame, 1), "result.counter");
        auto* stateIsDone = b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 0)), constant(2));
        auto* magicValid = b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic));
        auto* exact = b.CreateICmpEQ(committed, terminalBound, "result.exact");
        b.CreateCondBr(b.CreateAnd(stateIsDone, b.CreateAnd(magicValid, exact)),
                       valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        b.CreateRet(committed);
    }

    std::string errors;
    llvm::raw_string_ostream diagnostic(errors);
    if (llvm::verifyModule(module, &diagnostic))
        throw std::runtime_error("invalid scalar-loop LLVM IR: " + diagnostic.str());
    std::string output;
    llvm::raw_string_ostream o(output);
    module.print(o, nullptr);
    o.flush();
    return output;
}

} // namespace oreslang
