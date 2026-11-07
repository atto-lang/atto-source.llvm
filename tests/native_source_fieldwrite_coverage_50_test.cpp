#include "oreslang/backend/NativeIrFieldWriteLedger.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using oreslang::Statement;
using oreslang::Function;
using oreslang::Program;
using oreslang::native_ir::FieldWriteLedger;

struct Fixture {
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    Program program;
    Function* source_function = nullptr;
    llvm::Function* emitted_function = nullptr;
    llvm::BasicBlock* block = nullptr;
    llvm::Value* destination = nullptr;
    llvm::Value* alternate = nullptr;
    llvm::Value* stored_value = nullptr;
    llvm::StoreInst* store = nullptr;
    Statement* primary = nullptr;
    FieldWriteLedger ledger;

    Fixture() : module("direct-field-coverage", context), builder(context) {
        program.functions.reserve(8);
        program.functions.emplace_back();
        source_function = &program.functions.back();
        source_function->name = "guest";
        source_function->statements.reserve(128);
        source_function->caught_throw_defers.reserve(64);
        emitted_function = llvm::Function::Create(
            llvm::FunctionType::get(builder.getVoidTy(), false),
            llvm::Function::ExternalLinkage, "guest", module);
        block = llvm::BasicBlock::Create(context, "entry", emitted_function);
        builder.SetInsertPoint(block);
        auto* i64 = builder.getInt64Ty();
        destination = builder.CreateAlloca(i64, nullptr, "owner.x");
        alternate = builder.CreateAlloca(i64, nullptr, "owner.y");
        stored_value = llvm::ConstantInt::get(i64, 17);
        store = builder.CreateStore(stored_value, destination);
        builder.CreateRetVoid();
        primary = &add(*source_function, "owner", "x");
    }

    Statement& add(Function& function, const std::string& owner="owner",
            const std::string& field="y") {
        function.statements.emplace_back();
        auto& statement = function.statements.back();
        statement.kind = Statement::Kind::FieldWrite;
        statement.receiver = owner; statement.field = field;
        statement.offset = 41;
        return statement;
    }

    Statement& child(Statement& parent, bool then_arm=true,
            const std::string& field="y") {
        auto& statements = then_arm ? parent.then_statements : parent.else_statements;
        statements.reserve(64);
        statements.emplace_back();
        auto& statement = statements.back();
        statement.kind = Statement::Kind::FieldWrite;
        statement.receiver = "owner"; statement.field = field;
        statement.offset = 50;
        return statement;
    }

    Statement& wrapper(Statement::Kind kind=Statement::Kind::Conditional) {
        source_function->statements.emplace_back();
        auto& statement = source_function->statements.back();
        statement.kind = kind;
        return statement;
    }

    Statement& caught(const std::string& field="y") {
        source_function->caught_throw_defers.emplace_back();
        auto& statement = source_function->caught_throw_defers.back();
        statement.kind = Statement::Kind::FieldWrite;
        statement.receiver = "owner"; statement.field = field;
        statement.offset = 70;
        return statement;
    }

    llvm::StoreInst* another(llvm::Value* target=nullptr) {
        llvm::IRBuilder<> insertion(context);
        insertion.SetInsertPoint(block->getTerminator());
        return insertion.CreateStore(stored_value, target ? target : alternate);
    }

    void bind(Statement& statement, llvm::StoreInst* emitted=nullptr,
            llvm::Value* target=nullptr,
            const Function* parent=nullptr,
            const llvm::Function* ir_function=nullptr) {
        ledger.bind(statement, statement.receiver+"."+statement.field,
            target ? target : (emitted ? emitted->getPointerOperand():destination),
            emitted ? emitted : store,
            parent ? parent : source_function,
            ir_function ? ir_function : emitted_function);
    }

    void bind_primary() {bind(*primary);}
    void cover() const {(void)ledger.verify_source_coverage(program,module);}
    bool complete() const {
        const auto result = ledger.verify_source_coverage(program,module);
        return result.direct_source_writes_complete &&
            !result.ownership_complete && !result.optimized_code_verified &&
            !result.guest_verified;
    }
};

bool denied(const std::function<void()>& action) {
    try {action();}
    catch(const std::runtime_error& error) {
        return std::string(error.what()).find("ownership violation:") !=
            std::string::npos;
    }
    return false;
}
void check(bool yes, const char* label, unsigned& n) {
    ++n;
    if (!yes)
        throw std::runtime_error(std::string("case ")+std::to_string(n)+
            ": "+label);
}
int main() {
    unsigned n=0;
    try {
        check(denied([]{ Fixture f; f.cover(); }),"missing original source write",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function); f.cover(); }),"missing second direct field",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function); (void)f.add(*f.source_function); f.cover(); }),"missing third direct field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); (void)f.child(wrapper); f.cover(); }),"missing nested then branch field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); (void)f.child(wrapper,false); f.cover(); }),"missing nested else branch field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); (void)f.child(wrapper,true); (void)f.child(wrapper,false); f.cover(); }),"missing both conditional branch fields",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); auto& inner=wrapper.then_statements.emplace_back(); inner.kind=Statement::Kind::Conditional; (void)f.child(inner); f.cover(); }),"missing nested grandchild then field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); auto& inner=wrapper.else_statements.emplace_back(); inner.kind=Statement::Kind::Conditional; (void)f.child(inner,false); f.cover(); }),"missing nested grandchild else field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& loop=f.wrapper(Statement::Kind::ForIndex); (void)f.child(loop); f.cover(); }),"missing ForIndex body field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& loop=f.wrapper(Statement::Kind::ForEach); (void)f.child(loop); f.cover(); }),"missing ForEach body field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& loop=f.wrapper(Statement::Kind::ForEach); (void)f.child(loop,false); f.cover(); }),"missing ForEach else field",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& loop=f.wrapper(Statement::Kind::ForIndex); auto& inner=loop.then_statements.emplace_back(); inner.kind=Statement::Kind::Conditional; (void)f.child(inner); f.cover(); }),"missing early nested conditional in ForIndex",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.caught(); f.cover(); }),"missing caught throw cleanup field",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.caught(); (void)f.caught("z"); f.cover(); }),"missing two caught throw cleanup fields",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); other.statements.reserve(4); (void)f.add(other); f.cover(); }),"missing second Function's field",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); other.statements.reserve(4); auto& wrapper=other.statements.emplace_back(); wrapper.kind=Statement::Kind::Conditional; (void)f.child(wrapper); f.cover(); }),"missing second Function's nested field",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); other.caught_throw_defers.reserve(4); auto& st=other.caught_throw_defers.emplace_back(); st.kind=Statement::Kind::FieldWrite; st.receiver="owner"; st.field="y"; f.cover(); }),"missing second Function's caught throw field",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,"owner","late"); f.cover(); }),"added field after earlier complete binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); (void)f.child(wrapper); f.cover(); }),"added nested field after earlier complete binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.caught("late"); f.cover(); }),"added caught field after earlier complete binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,"","y"); f.cover(); }),"unbound source field with owner empty",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,"owner",""); f.cover(); }),"unbound source field with field empty",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,"owner.inner","y"); f.cover(); }),"unbound source owner contains projection dot",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,"owner","y.inner"); f.cover(); }),"unbound source field contains projection dot",n);
        check(denied([]{ Fixture f; f.bind_primary(); (void)f.add(*f.source_function,std::string(256,'o'),"z"); f.cover(); }),"unbound source path length exceeds 256",n);
        check(denied([]{ Fixture f; f.bind_primary(); Statement external{}; external.kind=Statement::Kind::FieldWrite; external.receiver="owner"; external.field="fake"; f.bind(external,f.another()); f.cover(); }),"forged source field with matching names but outside Program",n);
        check(denied([]{ Fixture f; f.bind_primary(); Statement external{}; external.kind=Statement::Kind::FieldWrite; external.receiver="owner"; external.field="y"; external.offset=0; f.bind(external,f.another()); f.cover(); }),"forged source field with zero offset outside Program",n);
        check(denied([]{ Fixture f; f.bind_primary(); Program foreign; f.ledger.verify_source_coverage(foreign,f.module); }),"source field bound to different Program",n);
        check(denied([]{ Fixture f; f.program.functions.emplace_back(); auto& other=f.program.functions.back(); f.bind(*f.primary,f.store,nullptr,&other,f.emitted_function); f.cover(); }),"source field bound to wrong source Function object",n);
        check(denied([]{ Fixture f; auto* alt=llvm::Function::Create(llvm::FunctionType::get(f.builder.getVoidTy(),false),llvm::Function::ExternalLinkage,"other",f.module); f.bind(*f.primary,f.store,nullptr,f.source_function,alt); }),"source field binding mismatches actual LLVM Function",n);
        check(denied([]{ Fixture f; f.another(f.destination); f.cover(); }),"source field omitted but equivalent extra physical store exists",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.primary->kind=Statement::Kind::Return; f.cover(); }),"source field source kind mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.primary->field="renamed"; f.cover(); }),"source field name mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.primary->receiver="foreign"; f.cover(); }),"source field owner mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); ++f.primary->offset; f.cover(); }),"source field source offset mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.primary->receiver.clear(); f.cover(); }),"source field invalid after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->setOperand(0,llvm::ConstantInt::get(f.builder.getInt64Ty(),19)); f.cover(); }),"source physical store value mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->setOperand(1,f.alternate); f.cover(); }),"source physical store destination mutated after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->eraseFromParent(); f.cover(); }),"source physical store erased after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->setMetadata("atto.ownership.field_write.source.v1",nullptr); f.cover(); }),"source physical store metadata erased after binding",n);
        check(denied([]{ Fixture f; f.bind_primary(); llvm::Module foreign("other",f.context); f.ledger.verify_source_coverage(f.program,foreign); }),"source binding missing actual store in foreign LLVM module",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); other.statements.reserve(4100); for(int i=0;i<4100;++i){auto& s=other.statements.emplace_back(); s.kind=Statement::Kind::Call;} f.cover(); }),"source inventory over 4096 operations",n);
        check([]{ Fixture f; f.bind_primary(); return f.complete(); }(),"one source field fully inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& s=f.add(*f.source_function); f.bind(s,f.another()); return f.complete() && f.ledger.size()==2; }(),"two direct fields inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"then branch field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(); auto& s=f.child(w,false); f.bind(s,f.another()); return f.complete(); }(),"else branch field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(Statement::Kind::ForIndex); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"ForIndex field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(Statement::Kind::ForEach); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"ForEach field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& s=f.caught(); f.bind(s,f.another()); return f.complete(); }(),"caught-throw field inventoried",n);
        check([]{ Fixture f; f.program.functions.clear(); auto result=f.ledger.verify_source_coverage(f.program,f.module); return result.direct_source_writes_complete && !result.source_to_unoptimized_ir_bound && !result.guest_verified; }(),"empty source/ledger is vacuous, nonpromoting",n);
        if(n != 50) throw std::runtime_error("wrong test inventory");
        std::cout<<"PASS 50/50 direct source FieldWrite completeness cases "
                    "(REAL LLVM IR HOST TEST; NOT NLL/GUEST VERIFIED)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL "<<n<<": "<<error.what()<<'\n';
        return 1;
    }
}
