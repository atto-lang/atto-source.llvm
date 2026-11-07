#include "oreslang/backend/NativeSourceFieldWriteLinks.hpp"
#include "oreslang/types/OwnershipCfg.hpp"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::native_ir;
namespace own=oreslang::ownership;
constexpr std::uint64_t domain=std::uint64_t{1}<<63;
bool denied(const std::function<void()>& op) {
    try {op();} catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:")!=std::string::npos;
    }
    return false;
}
void check(bool condition,const char* what,int& count) {
    if (!condition) throw std::runtime_error(std::string("case ")+
        std::to_string(count+1)+": "+what);
    ++count;
}
own::Graph restoration(std::uint64_t id) {
    own::Graph g;
    g.entry.emplace("p.x",own::PlaceState{own::kMoved});
    g.blocks.push_back({{{own::Operation::Kind::Reinitialize,
        "p.x",std::nullopt,id}},{}});
    return g;
}
int main() {
    int count=0;
    try {
        llvm::LLVMContext ctx;
        llvm::Module module("restore",ctx);
        auto* i64=llvm::Type::getInt64Ty(ctx);
        auto* void_ty=llvm::Type::getVoidTy(ctx);
        auto* fn=llvm::Function::Create(
            llvm::FunctionType::get(void_ty,{i64},false),
            llvm::Function::ExternalLinkage,"guest",module);
        auto* entry=llvm::BasicBlock::Create(ctx,"entry",fn);
        llvm::IRBuilder<> ir(entry);
        auto* arg=fn->getArg(0);
        auto* ptr=ir.CreateAlloca(i64);
        auto* store=ir.CreateStore(arg,ptr);
        ir.CreateRetVoid();
        const std::uint64_t valid=domain|1;
        own::verify(restoration(valid));
        auto snapshot=collect_native_ir_ssa_census(module);
        CompilerFieldWriteLinks ledger;
        ledger.record(*fn,"p.x",SourceFieldWriteKind::Direct,*store,valid);
        check(!llvm::verifyModule(module,&llvm::errs()),"LLVM verification",count);
        check(snapshot.stores.size()==1,"real StoreInst",count);
        check(ledger.sites().size()==1,"one site",count);
        check(ledger.sites()[0].reinitialize_write_id==valid,"ticket bound",count);
        check(ledger.sites()[0].destination==ptr,"destination bound",count);
        check(ledger.sites()[0].value==arg,"SSA payload bound",count);
        check(ledger.inspect(module,snapshot).physically_matched_writes==1,
            "actual module readback",count);
        check(!ledger.inspect(module,snapshot).source_ownership_complete &&
            !ledger.inspect(module,snapshot).guest_verified,
            "no guest/whole-source authority",count);
        // 8 distinct low-domain forged tickets.
        for(std::uint64_t id=1;id<=8;++id)
            check(denied([&]{CompilerFieldWriteLinks bad;
                bad.record(*fn,"p.x",SourceFieldWriteKind::Direct,
                    *store,id);}),"low-domain forgery",count);
        // 8 distinct high-domain IDs beyond the 4096-ticket quota.
        for(std::uint64_t id=4097;id<=4104;++id)
            check(denied([&]{CompilerFieldWriteLinks bad;
                bad.record(*fn,"p.x",SourceFieldWriteKind::Direct,
                    *store,domain|id);}),"quota bypass",count);
        // 8 wrong-kind claims cannot turn cleanup into a reinit witness.
        for(std::uint64_t id=1;id<=8;++id)
            check(denied([&]{CompilerFieldWriteLinks bad;
                bad.record(*fn,"p.x",
                    (id%2)==0 ? SourceFieldWriteKind::CatchCleanup :
                        SourceFieldWriteKind::Deferred,
                    *store,domain|id);}),"wrong emission category",count);
        // 8 malformed/unbounded field projection strings.
        const std::vector<std::string> bad_fields={
            "", ".x", "p.", "p..x", "..", ".", "x..y",
            std::string(257,'q')};
        for(const auto& field:bad_fields)
            check(denied([&]{CompilerFieldWriteLinks bad;
                bad.record(*fn,field,SourceFieldWriteKind::Direct,
                    *store,valid);}),"invalid field projection",count);
        // 8 independent physical/Cfg negative mutations.
        check(denied([&]{own::verify(restoration(0));}),
            "zero CFG ticket",count);
        check(denied([&]{auto g=restoration(valid);
            g.entry.at("p.x").possible=own::kOwned;
            own::verify(g);}),"unmoved restoration",count);
        check(denied([&]{auto g=restoration(valid);
            g.blocks[0].operations[0].place="p.y";own::verify(g);}),
            "wrong CFG destination",count);
        check(denied([&]{CompilerFieldWriteLinks bad;
            bad.record(*fn,"p.x",SourceFieldWriteKind::Direct,*store,valid);
            bad.record(*fn,"p.x",SourceFieldWriteKind::Direct,*store,valid);
        }),"duplicate source restoration ticket",count);
        auto* other=llvm::Function::Create(
            llvm::FunctionType::get(void_ty,false),
            llvm::Function::ExternalLinkage,"other",module);
        llvm::IRBuilder<> other_ir(
            llvm::BasicBlock::Create(ctx,"entry",other));
        other_ir.CreateRetVoid();
        check(denied([&]{CompilerFieldWriteLinks bad;
            bad.record(*other,"p.x",SourceFieldWriteKind::Direct,
                *store,valid);}),"wrong LLVM function",count);
        store->setOperand(0,llvm::ConstantInt::get(i64,9));
        check(denied([&]{(void)ledger.inspect(module,snapshot);}),
            "changed physical SSA value",count);
        CompilerFieldWriteLinks fresh;
        fresh.record(*fn,"p.x",SourceFieldWriteKind::Direct,*store,valid);
        const auto current=collect_native_ir_ssa_census(module);
        llvm::IRBuilder<> before(entry,entry->getTerminator()->getIterator());
        auto* extra=before.CreateStore(arg,ptr);
        check(denied([&]{(void)fresh.inspect(module,current);}),
            "new instruction invalidates stale snapshot",count);
        extra->eraseFromParent();
        store->eraseFromParent();
        check(denied([&]{(void)fresh.inspect(module,current);}),
            "erased StoreInst rejected without stale dereference",count);
        if(count!=48) throw std::runtime_error(
            "wrong number of cases: "+std::to_string(count));
        std::cout<<"PASS 48/48 real-LLVM restoration witness assertions "
                   "(HOST_ONLY; GUEST SSA NOT VERIFIED)\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"FAIL "<<count<<": "<<e.what()<<'\n';return 1;
    }
}
