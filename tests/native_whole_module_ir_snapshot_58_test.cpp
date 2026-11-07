#include "oreslang/backend/NativeSourceFieldWriteLinks.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::native_ir;

bool denied(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") !=
            std::string::npos;
    }
    return false;
}
void check(bool condition, const char* why, unsigned& total) {
    ++total;
    if (!condition)
        throw std::runtime_error(std::string("case ") +
            std::to_string(total) + ": " + why);
}

int main() {
    unsigned count=0;
    try {
        llvm::LLVMContext ctx;
        llvm::Module module("exact-whole-module-ir-receipt", ctx);
        auto* i64=llvm::Type::getInt64Ty(ctx);
        auto* i1=llvm::Type::getInt1Ty(ctx);
        auto* void_ty=llvm::Type::getVoidTy(ctx);
        auto* fn=llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {i64,i1}, false),
            llvm::Function::ExternalLinkage, "guest", module);
        const auto consume=module.getOrInsertFunction(
            "observe",llvm::FunctionType::get(void_ty,{i64},false));
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx,"entry",fn));
        auto* slot=b.CreateAlloca(i64,nullptr,"p.x");
        auto* store=b.CreateStore(fn->getArg(0),slot);
        std::vector<llvm::LoadInst*> loads;
        std::vector<llvm::CallInst*> calls;
        std::vector<llvm::BranchInst*> branches;
        std::vector<llvm::PHINode*> phis;
        std::vector<llvm::BasicBlock*> yes_blocks,no_blocks;
        for (unsigned i=0;i<10;i++) {
            auto* load=b.CreateLoad(i64,slot,"read"+std::to_string(i));
            auto* call=b.CreateCall(consume,{load});
            auto* yes=llvm::BasicBlock::Create(ctx,"yes"+std::to_string(i),fn);
            auto* no=llvm::BasicBlock::Create(ctx,"no"+std::to_string(i),fn);
            auto* join=llvm::BasicBlock::Create(ctx,"join"+std::to_string(i),fn);
            auto* branch=b.CreateCondBr(fn->getArg(1),yes,no);
            b.SetInsertPoint(yes);b.CreateBr(join);
            b.SetInsertPoint(no);b.CreateBr(join);
            b.SetInsertPoint(join);
            auto* phi=b.CreatePHI(i64,2,"merge"+std::to_string(i));
            phi->addIncoming(load,yes);
            phi->addIncoming(load,no);
            b.CreateCall(consume,{phi});
            loads.push_back(load);calls.push_back(call);
            branches.push_back(branch);phis.push_back(phi);
            yes_blocks.push_back(yes);no_blocks.push_back(no);
        }
        b.CreateRetVoid();
        check(!llvm::verifyModule(module,&llvm::errs()),
              "original 10-diamond SSA and PHI module verifies",count);
        const auto census=collect_native_ir_ssa_census(module);
        check(census.matches_current_module(),"actual IR text matches receipt",count);
        check(!census.module_ir_text.empty(),"bounded module text captured",count);
        check(census.module_ir_text.size() < 64*1024*1024,
              "evidence stays within 64MiB bound",count);
        check(census.stores.size()==1,"one physical source field store",count);
        check(census.phi_nodes==10 && census.phi_incoming==20,
              "ten real PHI instructions and twenty predecessor uses",count);
        CompilerFieldWriteLinks links;
        const std::uint64_t ticket=(std::uint64_t{1}<<63)|1;
        links.record(*fn,"p.x",SourceFieldWriteKind::Direct,*store,ticket);
        check(links.inspect(module,census).physically_matched_writes==1,
              "source restoration originally matches physical store",count);
        check(!census.ownership_complete && !census.guest_verified,
              "IR snapshot never claims complete loan or guest proof",count);

        // Each mutation is on a *different* real instruction or function
        // attribute. The source StoreInst and total instruction cardinality
        // stay unchanged, so the old store-only census would pass.
        auto probe=[&](const std::function<void()>& change,
                       const std::function<void()>& restore,
                       const char* reason) {
            const auto old=collect_native_ir_ssa_census(module);
            if (!old.matches_current_module())
                throw std::runtime_error("old census did not match at checkpoint");
            change();
            if (llvm::verifyModule(module,&llvm::errs()))
                throw std::runtime_error("mutation unexpectedly invalidated IR");
            check(!old.matches_current_module() &&
                  !old.contains_store(store,slot,fn->getArg(0)) &&
                  denied([&]{(void)links.inspect(module,old);}),
                  reason,count);
            restore();
            if (!old.matches_current_module())
                throw std::runtime_error("restored LLVM IR did not match exact snapshot");
        };
        for(unsigned i=0;i<10;i++) {
            auto* old=loads[i];
            probe([&,i]{calls[i]->setArgOperand(
                    0,llvm::ConstantInt::get(i64,100+i));},
                  [&,i,old]{calls[i]->setArgOperand(0,old);},
                  "non-store call SSA operand invalidates old receipt");
        }
        for(unsigned i=0;i<10;i++) {
            auto* old=loads[i];
            probe([&,i]{phis[i]->setIncomingValue(
                    0,llvm::ConstantInt::get(i64,200+i));},
                  [&,i,old]{phis[i]->setIncomingValue(0,old);},
                  "phi incoming SSA value invalidates old receipt");
        }
        for(unsigned i=0;i<10;i++) {
            probe([&,i]{
                      branches[i]->setSuccessor(0,no_blocks[i]);
                      branches[i]->setSuccessor(1,yes_blocks[i]);
                  },
                  [&,i]{
                      branches[i]->setSuccessor(0,yes_blocks[i]);
                      branches[i]->setSuccessor(1,no_blocks[i]);
                  },
                  "swapped valid branch successors invalidate old receipt");
        }
        for(unsigned i=0;i<10;i++) {
            const std::string tag="native.snapshot."+std::to_string(i);
            probe([&,i,&tag]{
                      loads[i]->setMetadata(tag,llvm::MDNode::get(
                          ctx,{llvm::MDString::get(ctx,"tag"+std::to_string(i))}));
                  },
                  [&,i,&tag]{loads[i]->setMetadata(tag,nullptr);},
                  "metadata-only change invalidates old ownership receipt");
        }
        for(unsigned i=0;i<10;i++) {
            const std::string attr="atto.snapshot."+std::to_string(i);
            probe([&]{fn->addFnAttr(attr,"temporary");},
                  [&]{fn->removeFnAttr(attr);},
                  "function attribute change invalidates old receipt");
        }
        if(count!=58)throw std::runtime_error(
            "expected 58 real-LLVM exact-IR checks, got "+std::to_string(count));
        std::cout<<"PASS 58/58 whole-module exact IR readback probes "
                   "(LLVM_HOST_IR_ONLY; SSA_LOAN_CLOSURE_FALSE)\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL "<<count<<": "<<e.what()<<'\n';return 1;
    }
}
