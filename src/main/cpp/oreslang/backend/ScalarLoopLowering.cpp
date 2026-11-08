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
        // Even though state=0 alone would permit initialization, accepting
        // dirty frame words can silently resurrect a recycled incarnation.
        // The ABI explicitly requires a fresh, zero-initialized owned frame.
        auto* freshState = b.CreateICmpEQ(b.CreateLoad(i64, stateAddress), constant(0));
        auto* freshCounter = b.CreateICmpEQ(
            b.CreateLoad(i64, field(b, frame, 1)), constant(0));
        auto* freshMagic = b.CreateICmpEQ(
            b.CreateLoad(i64, field(b, frame, 2)), constant(0));
        b.CreateCondBr(b.CreateAnd(freshState,
                                  b.CreateAnd(freshCounter, freshMagic)),
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
        // A corrupted actor frame must not claim DONE by setting its counter
        // past the source limit, nor run from a negative/stale counter. Check
        // once per dispatch BEFORE any guest reduction is committed.
        auto* countAtEntry = b.CreateLoad(i64, counterAddress, "frame.counter");
        auto* counterInRange = b.CreateAnd(
            b.CreateICmpSGE(countAtEntry, llvm::ConstantInt::getSigned(i64, loop.initial)),
            b.CreateICmpSLE(countAtEntry, llvm::ConstantInt::getSigned(i64, loop.upper_bound)));
        auto* verifiedCounter = llvm::BasicBlock::Create(
            context, "frame.counter.valid", step);
        b.CreateCondBr(counterInRange, verifiedCounter, invalid);
        b.SetInsertPoint(verifiedCounter);

        auto* active = llvm::BasicBlock::Create(context, "active", step);
        auto* done = llvm::BasicBlock::Create(context, "done", step);
        auto* cancelled = llvm::BasicBlock::Create(context, "cancelled", step);
        auto* loopHead = llvm::BasicBlock::Create(context, "loop.check", step);
        auto* budget = llvm::BasicBlock::Create(context, "loop.fuel", step);
        auto* batchStart = llvm::BasicBlock::Create(context, "batch.start", step);
        auto* advance = llvm::BasicBlock::Create(context, "loop.commit", step);
        auto* batchEnd = llvm::BasicBlock::Create(context, "batch.end", step);
        auto* exhausted = llvm::BasicBlock::Create(context, "cooperate", step);
        auto* complete = llvm::BasicBlock::Create(context, "loop.complete", step);
        auto* state = b.CreateLoad(i64, stateAddress, "state");
        // The scheduler consumes DONE without necessarily calling result().
        // A forged terminal state must therefore trap directly in step(),
        // even when its counter is otherwise within the legal range.
        auto* stateConsistent = b.CreateOr(
            b.CreateICmpNE(state, constant(2)),
            b.CreateICmpEQ(countAtEntry,
                llvm::ConstantInt::getSigned(i64, loop.upper_bound)));
        auto* stateVerified = llvm::BasicBlock::Create(
            context, "frame.state.consistent", step);
        b.CreateCondBr(stateConsistent, stateVerified, invalid);
        b.SetInsertPoint(stateVerified);
        auto* sw = b.CreateSwitch(state, invalid, 3);
        sw->addCase(constant(1), active);
        sw->addCase(constant(2), done);
        sw->addCase(constant(3), cancelled);

        b.SetInsertPoint(done);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(cancelled);
        b.CreateRet(llvm::ConstantInt::get(i32, 2));
        b.SetInsertPoint(active);
        b.CreateBr(loopHead);
        b.SetInsertPoint(loopHead);
        auto* current = b.CreateLoad(i64, counterAddress, "counter.before");
        auto* upperBound = llvm::ConstantInt::getSigned(i64, loop.upper_bound);
        b.CreateCondBr(b.CreateICmpSLT(current, upperBound), budget, complete);

        b.SetInsertPoint(complete);
        b.CreateStore(constant(2), stateAddress);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(budget);
        // Charge the dispatch fuel once per call, not on every backedge.
        // The source body is precisely a proven nonthrowing +1 effect.
        b.CreateCondBr(b.CreateICmpEQ(fuel, constant(0)), exhausted, batchStart);
        b.SetInsertPoint(batchStart);
        // 0 < fuel <= 512, current < upperBound <= 1e9; current + fuel
        // cannot signed-overflow even for an unexpectedly negative frame.
        auto* requestedEnd = b.CreateNSWAdd(current, fuel, "batch.requested_end");
        auto* batchLimit = b.CreateSelect(
            b.CreateICmpSLT(requestedEnd, upperBound), requestedEnd,
            upperBound, "batch.limit");
        b.CreateBr(advance);
        b.SetInsertPoint(exhausted);
        b.CreateRet(llvm::ConstantInt::get(i32, 1));
        b.SetInsertPoint(advance);
        auto* index = b.CreatePHI(i64, 2, "batch.current");
        index->addIncoming(current, batchStart);
        // Commit before deciding to return COOPERATE. The only in-loop test
        // is the native counted-loop boundary, not a runtime fuel/clock poll.
        auto* next = b.CreateNSWAdd(index, constant(1), "counter.committed");
        b.CreateStore(next, counterAddress);
        b.CreateCondBr(b.CreateICmpSLT(next, batchLimit), advance, batchEnd);
        index->addIncoming(next, advance);
        b.SetInsertPoint(batchEnd);
        // The last committed tick either completed the source loop or used
        // exactly the caller-approved fuel. No live guest native stack yields.
        b.CreateCondBr(b.CreateICmpEQ(next, upperBound), complete, exhausted);
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
        auto* counter = b.CreateLoad(i64, field(b, frame, 1));
        auto* counterInRange = b.CreateAnd(
            b.CreateICmpSGE(counter, llvm::ConstantInt::getSigned(i64, loop.initial)),
            b.CreateICmpSLE(counter, llvm::ConstantInt::getSigned(i64, loop.upper_bound)));
        auto* doneCounterValid = b.CreateOr(
            b.CreateICmpNE(s, constant(2)),
            b.CreateICmpEQ(counter,
                llvm::ConstantInt::getSigned(i64, loop.upper_bound)));
        b.CreateCondBr(b.CreateAnd(known,
            b.CreateAnd(counterInRange,
                b.CreateAnd(doneCounterValid, b.CreateICmpEQ(
                    b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic))))),
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
        // DONE is not enough to authorize a result: a forged terminal
        // state with an incomplete counter must trap instead of publishing
        // a silently truncated scalar guest result.
        auto* stateIsDone = b.CreateICmpEQ(
            b.CreateLoad(i64, field(b, frame, 0)), constant(2));
        auto* magicValid = b.CreateICmpEQ(
            b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic));
        auto* resultComplete = b.CreateICmpEQ(
            b.CreateLoad(i64, field(b, frame, 1)),
            llvm::ConstantInt::getSigned(i64, loop.upper_bound));
        b.CreateCondBr(b.CreateAnd(stateIsDone,
                                  b.CreateAnd(magicValid, resultComplete)),
                       valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        b.CreateRet(b.CreateLoad(i64, field(b, frame, 1)));
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
