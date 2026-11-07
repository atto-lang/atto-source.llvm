#include "oreslang/backend/NativeIrOwnershipCensus.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang::native_ir;

void check(bool good, const char* description, unsigned& count) {
    ++count;
    if (!good) throw std::runtime_error(std::string("case ") +
        std::to_string(count) + ": " + description);
}
bool denied(const std::function<void()>& op) {
    try { op(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") !=
            std::string::npos;
    }
    return false;
}

int main() {
    unsigned n = 0;
    try {
        llvm::LLVMContext context;
        llvm::Module module("real-ir-use-def-source", context);
        llvm::Type* i64 = llvm::Type::getInt64Ty(context);
        llvm::Type* void_type = llvm::Type::getVoidTy(context);
        auto* signature = llvm::FunctionType::get(void_type, {i64}, false);
        auto* function = llvm::Function::Create(signature,
            llvm::Function::ExternalLinkage, "guest", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        llvm::Argument* argument = function->getArg(0);
        llvm::Value* slot = builder.CreateAlloca(i64, nullptr, "owner.slot");
        llvm::StoreInst* store = builder.CreateStore(argument, slot);
        llvm::Value* loaded = builder.CreateLoad(i64, slot, "owner.read");
        (void)builder.CreateAdd(loaded, llvm::ConstantInt::get(i64, 1));
        builder.CreateRetVoid();
        check(!llvm::verifyModule(module, &llvm::errs()),
            "real in-memory LLVM module verifies", n);
        const auto inventory = collect_native_ir_ssa_census(module);
        check(inventory.instructions >= 4, "real instructions traversed", n);
        check(inventory.loads == 1, "real load was observed", n);
        check(inventory.stores.size() == 1, "real store was observed", n);
        check(inventory.ssa_operand_uses == inventory.reciprocal_ssa_uses,
              "LLVM SSA operand and use-list counts agree", n);
        check(inventory.ssa_operand_uses >= 4,
              "real SSA argument and load users counted", n);
        check(inventory.contains_store(store, slot, argument),
              "exact emitted instruction/destination/value matches", n);
        check(!inventory.contains_store(store, argument, argument),
              "wrong destination rejected", n);
        check(!inventory.contains_store(store, slot, loaded),
              "wrong stored SSA value rejected", n);
        check(!inventory.contains_store(nullptr, slot, argument),
              "anonymous instruction cannot impersonate a store", n);
        check(!inventory.contains_store(store, nullptr, argument),
              "anonymous destination rejected", n);
        check(!inventory.contains_store(store, slot, nullptr),
              "anonymous stored value rejected", n);
        check(!inventory.ownership_complete && !inventory.guest_verified,
              "LLVM census never promotes ownership/guest proof", n);
        check(inventory.stores.front().instruction_ordinal > 0,
              "real compiler instruction ordinal nonzero", n);
        llvm::StoreInst* second = nullptr;
        {
            llvm::IRBuilder<> before(builder.GetInsertBlock(),
                builder.GetInsertBlock()->getTerminator()->getIterator());
            second = before.CreateStore(argument, slot);
        }
        const auto two = collect_native_ir_ssa_census(module);
        check(two.stores.size() == 2 &&
              two.stores[0].instruction_ordinal <
              two.stores[1].instruction_ordinal,
              "two physical stores have different ordered sites", n);
        check(!inventory.contains_store(second, slot, argument),
              "earlier census cannot invent later emitted store", n);
        check(two.contains_store(second, slot, argument),
              "new store appears only in a new module walk", n);
        llvm::Value* replacement = llvm::ConstantInt::get(i64, 9);
        store->setOperand(0, replacement);
        check(!inventory.contains_store(store, slot, argument),
              "changed store operands invalidate cached census claim", n);
        const auto mutated = collect_native_ir_ssa_census(module);
        check(mutated.contains_store(store, slot, replacement),
              "new actual store value derived from mutated LLVM IR", n);
        check(!mutated.contains_store(store, slot, argument),
              "outdated value never authenticates current store", n);
        check(mutated.ssa_operand_uses == mutated.reciprocal_ssa_uses,
              "SSA census reconciles changed instruction operands", n);

        // A call is an actual emitted LLVM instruction, not a named JSON effect.
        llvm::FunctionCallee declared = module.getOrInsertFunction("external",
            llvm::FunctionType::get(void_type, false));
        {
            llvm::IRBuilder<> before(builder.GetInsertBlock(),
                builder.GetInsertBlock()->getTerminator()->getIterator());
            before.CreateCall(declared);
        }
        const auto call_record = collect_native_ir_ssa_census(module);
        check(call_record.calls == 1, "actual CallBase counted", n);
        check(call_record.stores.size() == 2,
              "declaration did not forge an extra store", n);

        // Build a separate actual phi CFG. Its incoming constants are not
        // misrepresented as SSA loan use origins.
        llvm::Module phi_module("phi-control",context);
        auto* phi_fn = llvm::Function::Create(
            llvm::FunctionType::get(i64,{llvm::Type::getInt1Ty(context)},false),
            llvm::Function::ExternalLinkage,"phi_control",phi_module);
        auto* start = llvm::BasicBlock::Create(context,"entry",phi_fn);
        auto* yes = llvm::BasicBlock::Create(context,"yes",phi_fn);
        auto* no = llvm::BasicBlock::Create(context,"no",phi_fn);
        auto* finish = llvm::BasicBlock::Create(context,"merge",phi_fn);
        llvm::IRBuilder<> branch(start);
        branch.CreateCondBr(phi_fn->getArg(0),yes,no);
        branch.SetInsertPoint(yes);
        branch.CreateBr(finish);
        branch.SetInsertPoint(no);
        branch.CreateBr(finish);
        branch.SetInsertPoint(finish);
        auto* phi = branch.CreatePHI(i64,2);
        phi->addIncoming(llvm::ConstantInt::get(i64,3),yes);
        phi->addIncoming(llvm::ConstantInt::get(i64,4),no);
        branch.CreateRet(phi);
        check(!llvm::verifyModule(phi_module,&llvm::errs()),
              "independent branch/phi module verifies", n);
        const auto phi_record = collect_native_ir_ssa_census(phi_module);
        check(phi_record.phi_nodes == 1 && phi_record.phi_incoming == 2,
              "compiler-owned phi edge inventory derived from real IR", n);
        check(phi_record.ssa_operand_uses == phi_record.reciprocal_ssa_uses,
              "phi SSA def-use edges reconcile", n);
        check(!phi_record.ownership_complete && !phi_record.guest_verified,
              "phi census does not authorize loan NLL", n);
        check(phi_record.stores.empty(),
              "phi-only IR cannot manufacture field-store evidence", n);

        // A malformed cross-function SSA use must not silently count as an
        // independent legal source owner. No verifier admission is claimed.
        llvm::Module foreign("foreign-SSA-use",context);
        auto* foreign_sig = llvm::FunctionType::get(void_type,{i64},false);
        auto* left=llvm::Function::Create(foreign_sig,
            llvm::Function::ExternalLinkage,"left",foreign);
        auto* right=llvm::Function::Create(foreign_sig,
            llvm::Function::ExternalLinkage,"right",foreign);
        llvm::IRBuilder<> left_builder(llvm::BasicBlock::Create(context,"entry",left));
        left_builder.CreateRetVoid();
        llvm::IRBuilder<> right_builder(llvm::BasicBlock::Create(context,"entry",right));
        auto* right_slot=right_builder.CreateAlloca(i64);
        right_builder.CreateStore(left->getArg(0),right_slot);
        right_builder.CreateRetVoid();
        check(denied([&] { (void)collect_native_ir_ssa_census(foreign); }),
              "foreign function SSA use fails closed", n);

        llvm::Module empty("no-guests", context);
        const auto empty_census = collect_native_ir_ssa_census(empty);
        check(empty_census.instructions == 0 && empty_census.stores.empty() &&
              !empty_census.ownership_complete,
              "empty module never produces phantom compiler evidence", n);
        if (n != 30) throw std::runtime_error("unexpected assertion count");
        std::cout << "PASS 30/30 actual LLVM IR SSA/store census checks "
                     "(REAL LLVM IR; HOST_ONLY, NOT ATTO GUEST OWNERSHIP)\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "FAIL at " << n << ": " << e.what() << '\n';
        return 1;
    }
}
