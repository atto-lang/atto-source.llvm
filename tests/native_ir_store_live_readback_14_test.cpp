#include "oreslang/backend/NativeIrOwnershipCensus.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang::native_ir;

void check(bool valid, const char* name, unsigned& count) {
    ++count;
    if (!valid) throw std::runtime_error(std::string("case ") +
        std::to_string(count) + ": " + name);
}

int main() {
    unsigned count = 0;
    try {
        llvm::LLVMContext context;
        llvm::Module module("live-llvm-store-readback", context);
        auto* i64 = llvm::Type::getInt64Ty(context);
        auto* void_ty = llvm::Type::getVoidTy(context);
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {i64}, false),
            llvm::Function::ExternalLinkage, "guest", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
        llvm::IRBuilder<> builder(entry);
        auto* arg = fn->getArg(0);
        auto* slot = builder.CreateAlloca(i64);
        auto* first = builder.CreateStore(arg, slot);
        builder.CreateRetVoid();
        check(!llvm::verifyModule(module, &llvm::errs()),
            "initial real LLVM module verifies", count);

        auto stale = collect_native_ir_ssa_census(module);
        check(stale.source_module == &module,
            "census is bound to actual module", count);
        check(stale.contains_store(first, slot, arg),
            "live physical StoreInst readback", count);
        check(!stale.ownership_complete && !stale.guest_verified,
            "no ownership or guest authority is granted", count);
        check(!stale.contains_store(first, slot, slot),
            "wrong stored SSA value cannot authenticate", count);

        auto* replacement = llvm::ConstantInt::get(i64, 73);
        first->setOperand(0, replacement);
        check(!stale.contains_store(first, slot, arg),
            "operand mutation invalidates old receipt", count);
        auto current = collect_native_ir_ssa_census(module);
        check(current.contains_store(first, slot, replacement),
            "new scan authenticates changed real store", count);

        llvm::IRBuilder<> before(entry,
            entry->getTerminator()->getIterator());
        auto* second = before.CreateStore(arg, slot);
        check(!current.contains_store(first, slot, replacement),
            "insertion invalidates earlier snapshot", count);
        auto two = collect_native_ir_ssa_census(module);
        check(two.stores.size() == 2 &&
            two.contains_store(first, slot, replacement),
            "new snapshot sees both physical instructions", count);
        check(two.contains_store(second, slot, arg),
            "second store matches only its live instruction", count);

        // Never dereference the erased StoreInst*. The old address is used
        // as an opaque equality candidate and the owning Module is rewalked.
        const llvm::StoreInst* erased = second;
        second->eraseFromParent();
        check(!two.contains_store(erased, slot, arg),
            "erased dangling instruction cannot authenticate a store", count);
        auto after_erase = collect_native_ir_ssa_census(module);
        check(after_erase.stores.size() == 1 &&
            after_erase.contains_store(first, slot, replacement),
            "fresh snapshot has only remaining live store", count);

        llvm::IRBuilder<> other_before(entry,
            entry->getTerminator()->getIterator());
        other_before.CreateCall(module.getOrInsertFunction("extern_func",
            llvm::FunctionType::get(void_ty, false)));
        check(!after_erase.contains_store(first, slot, replacement),
            "unrelated IR insertion also invalidates cached snapshot", count);

        llvm::Module foreign("separate-module", context);
        auto* foreign_fn = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {i64}, false),
            llvm::Function::ExternalLinkage, "other", foreign);
        llvm::IRBuilder<> foreign_builder(
            llvm::BasicBlock::Create(context, "entry", foreign_fn));
        auto* foreign_slot = foreign_builder.CreateAlloca(i64);
        auto* foreign_store = foreign_builder.CreateStore(
            foreign_fn->getArg(0), foreign_slot);
        foreign_builder.CreateRetVoid();
        check(!after_erase.contains_store(foreign_store,
                  foreign_slot, foreign_fn->getArg(0)),
            "foreign module store cannot impersonate this module", count);

        if (count != 14)
            throw std::runtime_error("wrong actual LLVM liveness test count");
        std::cout << "PASS 14/14 LLVM live store readback cases "
                     "(HOST_LLVM_IR_ONLY; NOT SSA-COMPLETE OR GUEST)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED after case " << count
                  << ": " << e.what() << '\n';
        return 1;
    }
}
