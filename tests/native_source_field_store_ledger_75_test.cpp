#include "oreslang/backend/NativeIrFieldWriteLedger.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace oreslang::native_ir;
using oreslang::Statement;

struct Fixture {
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    llvm::Type* i64 = nullptr;
    llvm::Function* function = nullptr;
    llvm::BasicBlock* block = nullptr;
    llvm::Value* destination = nullptr;
    llvm::Value* other = nullptr;
    llvm::Value* value = nullptr;
    llvm::StoreInst* store = nullptr;
    Statement statement{};
    FieldWriteLedger ledger;

    Fixture() : module("field-origin-control",context), builder(context) {
        i64=builder.getInt64Ty();
        function=llvm::Function::Create(
            llvm::FunctionType::get(builder.getVoidTy(),false),
            llvm::Function::ExternalLinkage,"field_writer",module);
        block=llvm::BasicBlock::Create(context,"entry",function);
        builder.SetInsertPoint(block);
        destination=builder.CreateAlloca(i64,nullptr,"owner.x");
        other=builder.CreateAlloca(i64,nullptr,"owner.y");
        value=llvm::ConstantInt::get(i64,17);
        store=builder.CreateStore(value,destination);
        builder.CreateRetVoid();
        statement.kind=Statement::Kind::FieldWrite;
        statement.receiver="owner";
        statement.field="x";
        statement.offset=13;
    }
    std::uint64_t bind(const std::string& path="owner.x") {
        return ledger.bind(statement,path,destination,store);
    }
    llvm::StoreInst* new_store(llvm::Value* v,llvm::Value* dst) {
        llvm::IRBuilder<> insert(context);
        insert.SetInsertPoint(block->getTerminator());
        return insert.CreateStore(v,dst);
    }
};

llvm::MDNode* provenance(llvm::LLVMContext& ctx,
        const std::string& key="owner.x",
        std::uint64_t ticket=1,std::uint64_t offset=13) {
    auto* i64=llvm::Type::getInt64Ty(ctx);
    return llvm::MDNode::get(ctx,{
        llvm::MDString::get(ctx,key),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,ticket)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,offset))
    });
}
bool denied(const std::function<void()>& action) {
    try {action();}
    catch(const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:")!=
            std::string::npos;
    }
    return false;
}
void check(bool passed,const char* name,std::size_t& count) {
    ++count;
    if(!passed)throw std::runtime_error(std::string("case ")+
        std::to_string(count)+": "+name);
}
int main() {
    std::size_t checks=0;
    try {
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::Call; f.bind(); }),"reject Call at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::ScalarWrite; f.bind(); }),"reject ScalarWrite at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::Defer; f.bind(); }),"reject Defer at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::Conditional; f.bind(); }),"reject Conditional at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::ScopedLocal; f.bind(); }),"reject ScopedLocal at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::Return; f.bind(); }),"reject Return at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::ForEach; f.bind(); }),"reject ForEach at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::ForIndex; f.bind(); }),"reject ForIndex at bind",checks);
        check(denied([]{ Fixture f; f.statement.kind=Statement::Kind::ArrayCommitFilled; f.bind(); }),"reject ArrayCommitFilled at bind",checks);
        check(denied([]{ Fixture f; f.statement.receiver.clear(); f.bind(); }),"reject empty source owner",checks);
        check(denied([]{ Fixture f; f.statement.field.clear(); f.bind(); }),"reject empty source field",checks);
        check(denied([]{ Fixture f; f.statement.receiver="owner.x"; f.bind(); }),"reject dotted owner",checks);
        check(denied([]{ Fixture f; f.statement.field="x.inner"; f.bind(); }),"reject dotted field",checks);
        check(denied([]{ Fixture f; f.bind("foreign.x"); }),"reject mismatched place owner",checks);
        check(denied([]{ Fixture f; f.bind("owner.y"); }),"reject mismatched place field",checks);
        check(denied([]{ Fixture f; f.bind("owner/x"); }),"reject mismatched place delimiter",checks);
        check(denied([]{ Fixture f; f.bind("owner"); }),"reject mismatched place omitted field",checks);
        check(denied([]{ Fixture f; f.bind("owner.x."); }),"reject mismatched place trailing dot",checks);
        check(denied([]{ Fixture f; f.statement.receiver=std::string(255,'p'); f.bind(f.statement.receiver+".x"); }),"reject canonical place length >256",checks);
        check(denied([]{ Fixture f; f.ledger.bind(f.statement,"owner.x",nullptr,f.store); }),"reject null destination",checks);
        check(denied([]{ Fixture f; f.ledger.bind(f.statement,"owner.x",f.destination,nullptr); }),"reject null StoreInst",checks);
        check(denied([]{ Fixture f; f.ledger.bind(f.statement,"owner.x",f.other,f.store); }),"reject swapped destination",checks);
        check(denied([]{ Fixture f; f.bind(); f.bind(); }),"reject duplicate source event",checks);
        check(denied([]{ Fixture f; f.bind(); Statement next{}; next.kind=Statement::Kind::FieldWrite; next.receiver="owner"; next.field="y"; f.ledger.bind(next,"owner.y",f.destination,f.store); }),"reject same store rebound to second source",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.kind=Statement::Kind::Return; f.ledger.verify(f.module); }),"reject source kind mutation after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.receiver="foreign"; f.ledger.verify(f.module); }),"reject source owner mutation after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.field="y"; f.ledger.verify(f.module); }),"reject source field mutation after bind",checks);
        check(denied([]{ Fixture f; f.bind(); ++f.statement.offset; f.ledger.verify(f.module); }),"reject source offset mutation after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.offset=0; f.ledger.verify(f.module); }),"reject source offset reset after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.receiver="owner.inner"; f.ledger.verify(f.module); }),"reject source new dotted owner after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.field="x.inner"; f.ledger.verify(f.module); }),"reject source new dotted field after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.field.clear(); f.ledger.verify(f.module); }),"reject source empty field after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.statement.receiver.clear(); f.ledger.verify(f.module); }),"reject source empty owner after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setOperand(1,f.other); f.ledger.verify(f.module); }),"reject mutated store pointer after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setOperand(0,llvm::ConstantInt::get(f.i64,99)); f.ledger.verify(f.module); }),"reject mutated store SSA value after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setOperand(0,llvm::ConstantInt::get(f.i64,18)); f.ledger.verify(f.module); }),"reject mutated store SSA argument after bind",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->eraseFromParent(); f.ledger.verify(f.module); }),"reject physically erased store",checks);
        check(denied([]{ Fixture f; f.bind(); llvm::Module wrong("foreign",f.context); f.ledger.verify(wrong); }),"reject receipt checked against wrong module",checks);
        check(denied([]{ Fixture f; f.bind(); llvm::Module wrong("empty",f.context); f.ledger.verify(wrong); }),"reject receipt checked against empty module",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->eraseFromParent(); f.new_store(f.value,f.destination); f.ledger.verify(f.module); }),"reject replaced store at same destination",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->eraseFromParent(); f.new_store(llvm::ConstantInt::get(f.i64,7),f.destination); f.ledger.verify(f.module); }),"reject replaced store with different value",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->eraseFromParent(); f.new_store(f.value,f.other); f.ledger.verify(f.module); }),"reject replaced store with different destination",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setOperand(0,llvm::ConstantInt::get(f.i64,42)); if(llvm::verifyModule(f.module,&llvm::errs()))throw std::runtime_error("invalid mutation"); f.ledger.verify(f.module); }),"reject different stored operand still valid LLVM",checks);
        check([]{ Fixture f; auto r=f.ledger.verify(f.module); return r.checked_source_stores==0 && !r.source_to_unoptimized_ir_bound && !r.ownership_complete && !r.optimized_code_verified && !r.guest_verified; }(),"empty ledger is non-promoting",checks);
        check([]{ Fixture f; return f.bind()==1; }(),"bind gives compiler-minted first ticket",checks);
        check([]{ Fixture f; f.bind(); auto r=f.ledger.verify(f.module); return r.checked_source_stores==1 && r.source_to_unoptimized_ir_bound; }(),"actual store can be verified",checks);
        check([]{ Fixture f; f.bind(); auto r=f.ledger.verify(f.module); return !r.ownership_complete && !r.optimized_code_verified && !r.guest_verified; }(),"no guest proof from source field binding",checks);
        check([]{ Fixture f; return !llvm::verifyModule(f.module,&llvm::errs()); }(),"valid LLVM module checks",checks);
        check([]{ Fixture f; f.statement.receiver="owner"; f.statement.field="x"; f.bind("owner.x"); return f.ledger.verify(f.module).checked_source_stores==1; }(),"single exact canonical path",checks);
        check([]{ Fixture f; f.statement.offset=0; f.bind(); return f.ledger.verify(f.module).checked_source_stores==1; }(),"source byte offset zero allowed",checks);
        check([]{ Fixture f; f.bind(); Statement other{}; other.kind=Statement::Kind::FieldWrite; other.receiver="owner"; other.field="y"; auto* store=f.new_store(f.value,f.other); auto ticket=f.ledger.bind(other,"owner.y",f.other,store); auto r=f.ledger.verify(f.module); return ticket==2 && r.checked_source_stores==2 && !r.ownership_complete; }(),"second real store with distinct source",checks);
        check([]{ Fixture f; f.statement.offset=918; f.bind(); return f.ledger.verify(f.module).checked_source_stores==1; }(),"different source offset allowed",checks);
        check([]{ Fixture f; f.statement.field="az_01"; f.bind("owner.az_01"); return f.ledger.verify(f.module).checked_source_stores==1; }(),"different valid named fields",checks);
        check([]{ Fixture f; f.statement.receiver="record"; f.bind("record.x"); return f.ledger.verify(f.module).checked_source_stores==1; }(),"different valid source owner",checks);
        check([]{ Fixture f; f.bind(); return f.ledger.verify(f.module).checked_source_stores==1; }(),"source-store binds to real pointer",checks);
        check([]{ Fixture f; return (f.bind()==1) && f.store->getMetadata("atto.ownership.field_write.source.v1") != nullptr; }(),"store carries compiler metadata",checks);
        check([]{ Fixture f; f.bind(); std::string text; llvm::raw_string_ostream o(text); f.module.print(o,nullptr); o.flush(); return text.find("atto.ownership.field_write.source.v1")!=std::string::npos; }(),"metadata survives in printed unoptimized LLVM IR",checks);
        check([]{ Fixture f; f.bind(); return f.ledger.verify(f.module).checked_source_stores==1; }(),"metadata verifies unchanged IR",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",nullptr); f.ledger.verify(f.module); }),"reject missing metadata",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner.y")); f.ledger.verify(f.module); }),"reject substituted metadata place",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"foreign.x")); f.ledger.verify(f.module); }),"reject substituted metadata owner",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner.x",2)); f.ledger.verify(f.module); }),"reject substituted metadata ticket",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner.x",0)); f.ledger.verify(f.module); }),"reject zero metadata ticket",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner.x",1,14)); f.ledger.verify(f.module); }),"reject substituted metadata offset",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner.x",1,0)); f.ledger.verify(f.module); }),"reject zero metadata offset if nonzero source",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{})); f.ledger.verify(f.module); }),"reject empty metadata node",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"owner.x")})); f.ledger.verify(f.module); }),"reject wrong metadata tuple length",checks);
        check(denied([]{ Fixture f; f.bind(); auto* v=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(f.i64,123)); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{v,v,v})); f.ledger.verify(f.module); }),"reject metadata non-string place",checks);
        check(denied([]{ Fixture f; f.bind(); auto* name=llvm::MDString::get(f.context,"owner.x"); auto* ticket=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context),1)); auto* offset=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(f.i64,13)); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{name,ticket,offset})); f.ledger.verify(f.module); }),"reject metadata ticket wrong integer width",checks);
        check(denied([]{ Fixture f; f.bind(); auto* name=llvm::MDString::get(f.context,"owner.x"); auto* ticket=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(f.i64,1)); auto* offset=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context),13)); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{name,ticket,offset})); f.ledger.verify(f.module); }),"reject metadata offset wrong integer width",checks);
        check(denied([]{ Fixture f; f.bind(); auto* tag=f.store->getMetadata("atto.ownership.field_write.source.v1"); f.store->setMetadata("other.provenance",tag); f.store->setMetadata("atto.ownership.field_write.source.v1",nullptr); f.ledger.verify(f.module); }),"reject metadata relocated to unrelated namespace",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"owner/x")); f.ledger.verify(f.module); }),"reject metadata with wrong field separator",checks);
        check(denied([]{ Fixture f; f.bind(); f.store->setMetadata("atto.ownership.field_write.source.v1",provenance(f.context,"")); f.ledger.verify(f.module); }),"reject empty metadata place",checks);
        check(denied([]{ Fixture f; f.bind(); auto* name=llvm::MDString::get(f.context,"owner.x"); auto* a=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(f.i64,1)); auto* b=llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(f.i64,13)); f.store->setMetadata("atto.ownership.field_write.source.v1",llvm::MDNode::get(f.context,{name,a,b,a})); f.ledger.verify(f.module); }),"reject metadata with unexpected extra operand",checks);
        check([]{ Fixture f; f.statement.offset=0; f.bind(); return f.ledger.verify(f.module).checked_source_stores==1; }(),"source offset 0 creates verifiable metadata",checks);
        if(checks != 75) throw std::runtime_error("incomplete mutation suite");
        std::cout<<"PASS 75/75 compiler-source -> actual LLVM field-store "
                    "binding cases (HOST_IR_ONLY; NO OPTIMIZED/GUEST NLL)\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAILED "<<checks<<": "<<e.what()<<'\n';
        return 1;
    }
}
