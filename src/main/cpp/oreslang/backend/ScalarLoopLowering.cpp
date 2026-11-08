#include "oreslang/backend/LlvmCodegen.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "oreslang/backend/LlvmCompat.hpp"
#include "oreslang/backend/MeteredCfgVerifier.hpp"
#include "oreslang/backend/ScalarGuestCallVerifier.hpp"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {

std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop) {
    if (loop.initial < 0 || loop.upper_bound < loop.initial ||
            loop.upper_bound > 1000000000LL)
        throw std::runtime_error("invalid experimental scalar loop bounds");

    // ABI v1 is intentionally named separately from the older 24-byte v0
    // symbols so a caller cannot silently pass an undersized legacy frame.
    // A trusted carrier allocates a pinned, exclusively owned 32-byte
    // [state, counter, magic, logical_fuel] heap frame. The logical budget
    // persists across every physical dispatch and cannot be refilled by
    // repeatedly supplying a new per-dispatch quantum. No native stack
    // survives the return-to-scheduler boundary. Actor dispatch is still a
    // separate, unimplemented security requirement.
    llvm::LLVMContext context;
    llvm::Module module("oreslang-native-scalar-loop", context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* voidTy = llvm::Type::getVoidTy(context);
    auto* frameTy = llvm::StructType::create(context, {i64, i64, i64, i64},
                                             "oreslang.rt.scalar.frame.v1");
    // The ABI's first parameter addresses the whole four-field frame.
    // On typed-pointer LLVM this MUST be frameTy*, not i64*: emitting
    // CreateStructGEP(frameTy, i64*, ...) is ill-typed on older toolchains.
    // Opaque-pointer LLVM retains the same C ABI while using ptr.
    auto* ptr = native_pointer_type(context, frameTy);
    constexpr std::uint64_t kMagic = 0x4f5245534c4f4f51ULL;
    constexpr std::uint64_t kMaxLogicalFuel = 100'000'000ULL;
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

    // ABI v1 explicitly admits one immutable logical fuel allowance per
    // guest frame; zero/oversized allocations trap before writing any bytes.
    // Callers may NOT replenish this quota on subsequent scheduler handoffs.
    auto* init = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr, i64}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_init_v1", module);
    {
        auto args = init->arg_begin();
        auto* frame = &*args++;
        auto* logicalFuel = &*args;
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", init));
        entryGuard(init, frame, b);
        auto* fuelValid = b.CreateAnd(
            b.CreateICmpUGT(logicalFuel, constant(0)),
            b.CreateICmpULE(logicalFuel, constant(kMaxLogicalFuel)));
        auto* approved = llvm::BasicBlock::Create(context, "fuel.approved", init);
        auto* denied = llvm::BasicBlock::Create(context, "fuel.invalid", init);
        b.CreateCondBr(fuelValid, approved, denied);
        b.SetInsertPoint(denied);
        fail(b);
        b.SetInsertPoint(approved);
        auto* stateAddress = field(b, frame, 0);
        auto* fresh = llvm::BasicBlock::Create(context, "fresh", init);
        auto* invalid = llvm::BasicBlock::Create(context, "already.initialized", init);
        // State=0 alone is not proof of a virgin frame. Require all four
        // fields zero so an accidentally reset state cannot reinitialize a
        // live or terminal task. This detects corruption, not hostile writes
        // to guest-accessible memory: the host must own the entire frame.
        llvm::Value* pristine = b.CreateICmpEQ(
            b.CreateLoad(i64, stateAddress), constant(0));
        for (unsigned fieldIndex = 1; fieldIndex != 4; ++fieldIndex) {
            pristine = b.CreateAnd(pristine, b.CreateICmpEQ(
                b.CreateLoad(i64, field(b, frame, fieldIndex)), constant(0)));
        }
        b.CreateCondBr(pristine, fresh, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(fresh);
        b.CreateStore(constant(1), stateAddress);
        b.CreateStore(constant(static_cast<std::uint64_t>(loop.initial)), field(b, frame, 1));
        b.CreateStore(constant(kMagic), field(b, frame, 2));
        b.CreateStore(logicalFuel, field(b, frame, 3));
        b.CreateRetVoid();
    }

    // Status: 0 DONE, 1 COOPERATE, 2 CANCELLED, 3 LOGICAL_FUEL_EXHAUSTED.
    // Zero physical fuel requests an ordinary handoff while logical fuel
    // remains. An exhausted tenant cannot evade terminal quota denial by
    // repeatedly requesting zero physical fuel. Fuel >512 traps.
    // On a completed loop, DONE wins even when the last unit of fuel is used.
    auto* step = llvm::Function::Create(
        llvm::FunctionType::get(i32, {ptr, i64}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_step_v1", module);
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
        auto* expired = llvm::BasicBlock::Create(context, "fuel.expired", step);
        auto* loopHead = llvm::BasicBlock::Create(context, "loop.check", step);
        auto* budget = llvm::BasicBlock::Create(context, "loop.fuel", step);
        auto* advance = llvm::BasicBlock::Create(context, "loop.commit", step);
        auto* exhausted = llvm::BasicBlock::Create(context, "cooperate", step);
        auto* logicalReady = llvm::BasicBlock::Create(context, "fuel.logical.available", step);
        auto* logicalExhausted = llvm::BasicBlock::Create(context, "fuel.logical.exhausted", step);
        auto* complete = llvm::BasicBlock::Create(context, "loop.complete", step);
        // The frame is trusted-carrier-owned, but reject accidental or
        // adversarially malformed counters and lifetime fuel before a fake
        // completed state can bypass any work. This cannot distinguish a
        // plausible guest-forged refill from legitimate host state.
        auto* state = b.CreateLoad(i64, stateAddress, "state");
        auto* storedCounter = b.CreateLoad(i64, counterAddress, "frame.counter.validated");
        auto* storedFuel = b.CreateLoad(i64, field(b, frame, 3), "frame.fuel.validated");
        auto* validCounter = b.CreateAnd(
            b.CreateICmpUGE(storedCounter,
                constant(static_cast<std::uint64_t>(loop.initial))),
            b.CreateICmpULE(storedCounter,
                constant(static_cast<std::uint64_t>(loop.upper_bound))));
        auto* validFuel = b.CreateICmpULE(storedFuel, constant(kMaxLogicalFuel));
        auto* doneConsistent = b.CreateOr(
            b.CreateICmpNE(state, constant(2)),
            b.CreateICmpEQ(storedCounter,
                constant(static_cast<std::uint64_t>(loop.upper_bound))));
        auto* expiredConsistent = b.CreateOr(
            b.CreateICmpNE(state, constant(4)),
            b.CreateAnd(b.CreateICmpEQ(storedFuel, constant(0)),
                b.CreateICmpULT(storedCounter,
                    constant(static_cast<std::uint64_t>(loop.upper_bound)))));
        auto* stateChecked = llvm::BasicBlock::Create(context, "frame.state.checked", step);
        b.CreateCondBr(b.CreateAnd(
            b.CreateAnd(validCounter, validFuel),
            b.CreateAnd(doneConsistent, expiredConsistent)), stateChecked, invalid);
        b.SetInsertPoint(stateChecked);
        auto* sw = b.CreateSwitch(state, invalid, 4);
        sw->addCase(constant(1), active);
        sw->addCase(constant(2), done);
        sw->addCase(constant(3), cancelled);
        sw->addCase(constant(4), expired);

        b.SetInsertPoint(done);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(cancelled);
        b.CreateRet(llvm::ConstantInt::get(i32, 2));
        b.SetInsertPoint(expired);
        b.CreateRet(llvm::ConstantInt::get(i32, 3));
        b.SetInsertPoint(active);
        b.CreateBr(loopHead);
        b.SetInsertPoint(loopHead);
        auto* remaining = b.CreatePHI(i64, 2, "dispatch.fuel");
        remaining->addIncoming(fuel, active);
        auto* current = b.CreateLoad(i64, counterAddress, "counter.before");
        b.CreateCondBr(b.CreateICmpSLT(current,
            llvm::ConstantInt::getSigned(i64, loop.upper_bound)), budget, complete);

        b.SetInsertPoint(complete);
        b.CreateStore(constant(2), stateAddress);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(budget);
        // Check the lifetime quota BEFORE the physical dispatch slice:
        // repeated step(frame, 0) cannot mask a depleted logical tenant.
        auto* logicalFuelAddress = field(b, frame, 3);
        auto* logicalRemaining = b.CreateLoad(i64, logicalFuelAddress, "logical.remaining");
        b.CreateCondBr(b.CreateICmpEQ(logicalRemaining, constant(0)),
                       logicalExhausted, logicalReady);
        b.SetInsertPoint(logicalExhausted);
        b.CreateStore(constant(4), stateAddress);
        b.CreateRet(llvm::ConstantInt::get(i32, 3));
        b.SetInsertPoint(logicalReady);
        b.CreateCondBr(b.CreateICmpEQ(remaining, constant(0)), exhausted, advance);
        b.SetInsertPoint(exhausted);
        b.CreateRet(llvm::ConstantInt::get(i32, 1));
        b.SetInsertPoint(advance);
        // Source body has exactly one nonthrowing, bounded +1 effect.
        // Commit the i64 counter BEFORE any subsequent cooperation request.
        auto* next = b.CreateNSWAdd(current, constant(1), "counter.committed");
        b.CreateStore(next, counterAddress);
        auto* nextLogical = b.CreateSub(logicalRemaining, constant(1),
                                        "fuel.remaining.logical");
        auto* logicalFuelDebit = b.CreateStore(nextLogical, logicalFuelAddress);
        auto* nextFuel = b.CreateSub(remaining, constant(1), "fuel.remaining.dispatch");
        b.CreateBr(loopHead);
        remaining->addIncoming(nextFuel, advance);

        // Fail closed at the IR construction boundary: no generated cycle
        // may bypass the committed lifetime-fuel debit. This independent
        // graph proof also catches a future newly-added loop backedge that
        // accidentally bypasses the metered block. It is not a substitute
        // for optimized AOT execution or recursive/call-graph certification.
        if (logicalFuelDebit->getParent() != advance ||
                logicalFuelDebit->getPointerOperand() != logicalFuelAddress ||
                logicalFuelDebit->getValueOperand() != nextLogical)
            throw std::runtime_error("scalar guest metering debit is not in committed block");
        std::unordered_map<const llvm::BasicBlock*, std::size_t> ids;
        ids.reserve(step->size());
        std::size_t index = 0;
        for (const auto& block : *step) ids.emplace(&block, index++);
        std::vector<std::vector<std::size_t>> edges(step->size());
        for (const auto& block : *step) {
            const auto from = ids.at(&block);
            for (const auto* successor : llvm::successors(&block)) {
                const auto found = ids.find(successor);
                if (found == ids.end())
                    throw std::runtime_error("scalar guest CFG has foreign successor");
                edges[from].push_back(found->second);
            }
        }
        if (!backend_detail::every_cycle_contains_meter(edges, {ids.at(advance)}))
            throw std::runtime_error("scalar guest CFG has a cycle without lifetime fuel debit");
    }

    auto* cancel = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_cancel_v1", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", cancel));
        auto* frame = &*cancel->arg_begin();
        entryGuard(cancel, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* s = b.CreateLoad(i64, stateAddress);
        auto* valid = llvm::BasicBlock::Create(context, "valid", cancel);
        auto* invalid = llvm::BasicBlock::Create(context, "invalid", cancel);
        auto* known = b.CreateAnd(b.CreateICmpUGE(s, constant(1)),
                                   b.CreateICmpULE(s, constant(4)));
        auto* counter = b.CreateLoad(i64, field(b, frame, 1));
        auto* quota = b.CreateLoad(i64, field(b, frame, 3));
        auto* range = b.CreateAnd(
            b.CreateICmpUGE(counter,
                constant(static_cast<std::uint64_t>(loop.initial))),
            b.CreateICmpULE(counter,
                constant(static_cast<std::uint64_t>(loop.upper_bound))));
        auto* doneConsistent = b.CreateOr(
            b.CreateICmpNE(s, constant(2)),
            b.CreateICmpEQ(counter,
                constant(static_cast<std::uint64_t>(loop.upper_bound))));
        auto* expiredConsistent = b.CreateOr(
            b.CreateICmpNE(s, constant(4)),
            b.CreateAnd(b.CreateICmpEQ(quota, constant(0)),
                b.CreateICmpULT(counter,
                    constant(static_cast<std::uint64_t>(loop.upper_bound)))));
        auto* validShape = b.CreateAnd(
            b.CreateAnd(known, range),
            b.CreateAnd(b.CreateICmpULE(quota, constant(kMaxLogicalFuel)),
                b.CreateAnd(doneConsistent, expiredConsistent)));
        b.CreateCondBr(b.CreateAnd(validShape,
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic))),
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
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_result_v1", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", result));
        auto* frame = &*result->arg_begin();
        entryGuard(result, frame, b);
        auto* valid = llvm::BasicBlock::Create(context, "result.ready", result);
        auto* invalid = llvm::BasicBlock::Create(context, "result.invalid", result);
        auto* completedCounter = b.CreateLoad(i64, field(b, frame, 1));
        auto* remainingFuel = b.CreateLoad(i64, field(b, frame, 3));
        auto* validDone = b.CreateAnd(
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 0)), constant(2)),
            b.CreateAnd(
                b.CreateICmpEQ(completedCounter,
                    constant(static_cast<std::uint64_t>(loop.upper_bound))),
                b.CreateICmpULE(remainingFuel, constant(kMaxLogicalFuel))));
        b.CreateCondBr(b.CreateAnd(validDone,
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic))),
            valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        b.CreateRet(b.CreateLoad(i64, field(b, frame, 1)));
    }

    backend_detail::require_closed_scalar_guest_calls(module);
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
