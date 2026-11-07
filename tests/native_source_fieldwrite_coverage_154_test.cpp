#include "oreslang/backend/NativeIrFieldWriteLedger.hpp"
#include "oreslang/backend/NativeIrReinitializeWriteBridge.hpp"
#include "oreslang/backend/NativeIrSourceCheckedReinitialize.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
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
using oreslang::native_ir::CandidateReinitializeAudit;
using oreslang::native_ir::verify_candidate_reinitialize_store_bridge;
using oreslang::ownership::Graph;
using oreslang::ownership::Operation;
using oreslang::ownership::PlaceState;

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
    // Builds the independent source-Function -> actual LLVM Function map used
    // by production codegen from its declaration registry. Extra test source
    // functions receive genuine separate LLVM function bodies, so missing
    // field-write mutations do not pass merely due to a missing declaration.
    std::map<const Function*, const llvm::Function*> function_map() {
        std::map<const Function*, const llvm::Function*> result;
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            if (i == 0) {
                result.emplace(&program.functions[i], emitted_function);
                continue;
            }
            const auto name = std::string("guest_extra_") + std::to_string(i);
            llvm::Function* extra = module.getFunction(name);
            if (!extra) {
                extra = llvm::Function::Create(
                    llvm::FunctionType::get(builder.getVoidTy(), false),
                    llvm::Function::ExternalLinkage, name, module);
                llvm::IRBuilder<> body(
                    llvm::BasicBlock::Create(context, "entry", extra));
                body.CreateRetVoid();
            }
            result.emplace(&program.functions[i], extra);
        }
        return result;
    }
    void cover() {
        (void)ledger.verify_source_coverage(program,module,function_map());
    }
    bool complete() {
        const auto result =
            ledger.verify_source_coverage(program,module,function_map());
        return result.direct_source_writes_complete &&
            !result.ownership_complete && !result.optimized_code_verified &&
            !result.guest_verified;
    }
};

// Candidate CFGs here are synthetic host fixtures, NOT compiler-built typed
// MIR. The bridge must never upgrade them to guest ownership authority.
Graph candidate_graph(std::uint64_t ticket=1,
        const std::string& place="owner.x",
        oreslang::ownership::ValueMask initial=oreslang::ownership::kMoved) {
    Graph g;
    g.entry.emplace(place, PlaceState(initial));
    g.blocks.resize(1);
    g.blocks[0].operations.emplace_back(
        Operation::Kind::Reinitialize, place, std::nullopt, ticket);
    return g;
}
CandidateReinitializeAudit candidate(Fixture& f, Graph g) {
    std::map<const Function*, Graph> graphs;
    graphs.emplace(f.source_function, std::move(g));
    return verify_candidate_reinitialize_store_bridge(
        f.ledger, f.program, f.module, f.function_map(), graphs);
}
bool safe_candidate(CandidateReinitializeAudit result) {
    return result.checked_claims != 0 &&
        result.candidate_write_id_store_bound &&
        !result.typed_mir_complete &&
        !result.full_loan_use_coverage &&
        !result.optimized_code_verified &&
        !result.guest_verified;
}

// Synthetic host mutation of a checker-issued flag. Real production
// authorization is minted by validate_i64_ownership, never by this fixture.
// This only tests the sealed identity + live StoreInst verifier.
void seal_direct_write(Fixture& f, bool reinitialize) {
    f.primary->compiler_checked_field_reinitialization = reinitialize;
    oreslang::source_identity::seal_validated_program(f.program);
    f.ledger = FieldWriteLedger(f.program);
    f.bind_primary();
}
oreslang::native_ir::SourceCheckedReinitializeAudit direct_reinit(Fixture& f) {
    return oreslang::native_ir::verify_source_checked_reinitialize_stores(
        f.ledger, f.program, f.module, f.function_map());
}
bool bounded_only(oreslang::native_ir::SourceCheckedReinitializeAudit a) {
    return a.checked_moved_field_writes == 1 &&
        a.bounded_source_checked_stores_bound &&
        !a.full_typed_mir_complete &&
        !a.full_loan_use_coverage &&
        !a.optimized_code_verified &&
        !a.guest_verified;
}

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
        check(denied([]{ Fixture f; f.bind_primary(); Program foreign; f.ledger.verify_source_coverage(foreign,f.module,f.function_map()); }),"source field bound to different Program",n);
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
        check(denied([]{ Fixture f; f.bind_primary(); llvm::Module foreign("other",f.context); f.ledger.verify_source_coverage(f.program,foreign,f.function_map()); }),"source binding missing actual store in foreign LLVM module",n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); other.statements.reserve(4100); for(int i=0;i<4100;++i){auto& s=other.statements.emplace_back(); s.kind=Statement::Kind::Call;} f.cover(); }),"source inventory over 4096 operations",n);
        check([]{ Fixture f; f.bind_primary(); return f.complete(); }(),"one source field fully inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& s=f.add(*f.source_function); f.bind(s,f.another()); return f.complete() && f.ledger.size()==2; }(),"two direct fields inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"then branch field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(); auto& s=f.child(w,false); f.bind(s,f.another()); return f.complete(); }(),"else branch field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(Statement::Kind::ForIndex); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"ForIndex field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& w=f.wrapper(Statement::Kind::ForEach); auto& s=f.child(w); f.bind(s,f.another()); return f.complete(); }(),"ForEach field inventoried",n);
        check([]{ Fixture f; f.bind_primary(); auto& s=f.caught(); f.bind(s,f.another()); return f.complete(); }(),"caught-throw field inventoried",n);
        check([]{ Fixture f; f.program.functions.clear(); auto result=f.ledger.verify_source_coverage(f.program,f.module,f.function_map()); return !result.direct_source_writes_complete && !result.source_to_unoptimized_ir_bound && !result.guest_verified; }(),"empty source/ledger is vacuous, nonpromoting",n);
        check(denied([]{ Fixture f; f.bind_primary(); std::map<const Function*,const llvm::Function*> missing; f.ledger.verify_source_coverage(f.program,f.module,missing); }), "reject missing authoritative function declaration", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto map=f.function_map(); Function outside; map.emplace(&outside,f.emitted_function); f.ledger.verify_source_coverage(f.program,f.module,map); }), "reject forged extra source declaration", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto map=f.function_map(); map.erase(&f.program.functions.back()); f.ledger.verify_source_coverage(f.program,f.module,map); }), "reject omitted second source function declaration", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto map=f.function_map(); map[&f.program.functions.back()]=f.emitted_function; f.ledger.verify_source_coverage(f.program,f.module,map); }), "reject two source functions aliased to one LLVM function", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* alternative=llvm::Function::Create(llvm::FunctionType::get(f.builder.getVoidTy(),false),llvm::Function::ExternalLinkage,"wrong",f.module); llvm::IRBuilder<> b(llvm::BasicBlock::Create(f.context,"entry",alternative)); b.CreateRetVoid(); auto map=f.function_map(); map[f.source_function]=alternative; f.ledger.verify_source_coverage(f.program,f.module,map); }), "reject mapped LLVM function different from store's real owner", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* declaration=llvm::Function::Create(llvm::FunctionType::get(f.builder.getVoidTy(),false),llvm::Function::ExternalLinkage,"external_only",f.module); auto map=f.function_map(); map[f.source_function]=declaration; f.ledger.verify_source_coverage(f.program,f.module,map); }), "reject source declaration without emitted body", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* copied=f.another(); copied->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); f.cover(); }), "reject unpaired duplicated tagged store", n);
        check(denied([]{ Fixture f; f.program.functions.clear(); auto* tag=llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"owner.x")}); f.store->setMetadata("atto.ownership.field_write.source.v1",tag); f.cover(); }), "reject tagged store with empty source inventory", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* extra=llvm::Function::Create(llvm::FunctionType::get(f.builder.getVoidTy(),false),llvm::Function::ExternalLinkage,"forged_extra",f.module); llvm::IRBuilder<> b(llvm::BasicBlock::Create(f.context,"entry",extra)); auto* slot=b.CreateAlloca(b.getInt64Ty()); auto* tagged=b.CreateStore(llvm::ConstantInt::get(b.getInt64Ty(),4),slot); tagged->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); b.CreateRetVoid(); f.cover(); }), "reject forged tagged store in separate LLVM function", n);
        check(denied([]{ Fixture f; f.bind_primary(); for(int i=0;i<3;++i){auto* tagged=f.another();tagged->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1"));}f.cover(); }), "reject three unpaired tagged stores despite one ledger entry", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* tagged=f.another(); tagged->setOperand(0,llvm::ConstantInt::get(f.builder.getInt64Ty(),999)); tagged->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); f.cover(); }), "reject unpaired differently valued tagged store", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto* forged=f.another(f.destination); forged->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); f.cover(); }), "reject substituted source ledger tag on unbound destination", n);
        check([]{ Fixture f; f.bind_primary(); auto map=f.function_map(); auto proof=f.ledger.verify_source_coverage(f.program,f.module,map); return map.size()==1 && proof.direct_source_writes_complete && !proof.ownership_complete && !proof.optimized_code_verified && !proof.guest_verified; }(), "authoritative source mapping keeps one bound write", n);
        check([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto map=f.function_map(); return map.size()==2 && map.begin()->second!=std::next(map.begin())->second && f.ledger.verify_source_coverage(f.program,f.module,map).direct_source_writes_complete; }(), "independent second native function with zero writes is allowed", n);
        check([]{ Fixture f; f.bind_primary(); (void)f.another(); return f.complete(); }(), "plain untagged housekeeping store remains admissible", n);
        check([]{ Fixture f; f.bind_primary(); auto* helper=llvm::Function::Create(llvm::FunctionType::get(f.builder.getVoidTy(),false),llvm::Function::ExternalLinkage,"internal_helper",f.module); llvm::IRBuilder<> b(llvm::BasicBlock::Create(f.context,"entry",helper)); auto* slot=b.CreateAlloca(b.getInt64Ty()); b.CreateStore(llvm::ConstantInt::get(b.getInt64Ty(),0),slot); b.CreateRetVoid(); return f.complete(); }(), "separate runtime LLVM helper untagged store remains admissible", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.clear(); f.cover(); }), "reject deleted source Program after binding", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.source_function->statements.clear(); f.cover(); }), "reject deleted source Statement after binding", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.source_function->statements.erase(f.source_function->statements.begin()); f.cover(); }), "reject erased source Statement after binding", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.clear(); f.program.functions.emplace_back(); f.cover(); }), "reject replaced source Function after binding", n);
        check(denied([]{ Fixture f; f.bind_primary(); {Statement temp{}; temp.kind=Statement::Kind::FieldWrite; temp.receiver="owner"; temp.field="z"; f.bind(temp,f.another()); } f.cover(); }), "reject stale temporary source Statement after scope end", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.clear(); f.store->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); f.cover(); }), "reject cleared source function plus remaining LLVM tag", n);
        check([]{ Fixture f; f.bind_primary(); auto& wrapper=f.wrapper(); auto& yes=f.child(wrapper,true,"y"); auto& no=f.child(wrapper,false,"z"); f.bind(yes,f.another()); f.bind(no,f.another()); return f.complete() && f.ledger.size()==3; }(), "accept both conditional branches fully covered", n);
        check([]{ Fixture f; f.bind_primary(); auto& action=f.add(*f.source_function); auto& caught=f.caught(); f.bind(action,f.another()); f.bind(caught,f.another()); return f.complete() && f.ledger.size()==3; }(), "accept independent direct and caught cleanup field", n);
        check(denied([]{ Fixture f; candidate(f,candidate_graph()); }), "missing source-to-store ledger fails bridge", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(0)); }), "zero write ID rejected", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations[0].write_id.reset(); candidate(f,g); }), "missing write ID rejected", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(100)); }), "nonexistent write ID rejected", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.y")); }), "wrong field refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"foreign.x")); }), "wrong owning object refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.x.y")); }), "wrong nested field refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& s=f.add(*f.source_function); f.bind(s,f.another()); candidate(f,candidate_graph(2,"owner.x")); }), "wrong ticket for first field refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto& s=f.add(*f.source_function); f.bind(s,f.another()); candidate(f,candidate_graph(2,"owner.x")); }), "wrong field for second ticket refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.x",oreslang::ownership::kOwned)); }), "reinitialization on already owned place refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.x",oreslang::ownership::kDropped)); }), "reinitialization on dropped place refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.x",oreslang::ownership::kUninitialized)); }), "reinitialization on uninitialized place refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); candidate(f,candidate_graph(1,"owner.x",static_cast<oreslang::ownership::ValueMask>(oreslang::ownership::kOwned|oreslang::ownership::kMoved))); }), "reinitialization on ambiguous place refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations.push_back({Operation::Kind::Reinitialize,"owner.x",std::nullopt,1}); candidate(f,g); }), "duplicate ticket in same block refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations[0].loan_id=7; candidate(f,g); }), "loan token grafted onto reinitialization refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks.clear(); candidate(f,g); }), "reinitialization graph with no blocks refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].successors={9}; candidate(f,g); }), "bad CFG successor refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations.clear(); g.blocks.resize(2); g.blocks[1].operations.emplace_back(Operation::Kind::Reinitialize,"owner.x",std::nullopt,1); candidate(f,g); }), "unreachable reinitialization refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); std::map<const Function*,Graph> empty; verify_candidate_reinitialize_store_bridge(f.ledger,f.program,f.module,f.function_map(),empty); }), "empty function graph inventory refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); Function outside; std::map<const Function*,Graph> graphs; graphs.emplace(&outside,candidate_graph()); verify_candidate_reinitialize_store_bridge(f.ledger,f.program,f.module,f.function_map(),graphs); }), "foreign function graph refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); std::map<const Function*,Graph> graphs; graphs.emplace(f.source_function,candidate_graph()); verify_candidate_reinitialize_store_bridge(f.ledger,f.program,f.module,f.function_map(),graphs); }), "missing additional source function graph refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.program.functions.emplace_back(); auto& other=f.program.functions.back(); std::map<const Function*,Graph> graphs; graphs.emplace(f.source_function,candidate_graph()); graphs.emplace(&other,candidate_graph()); verify_candidate_reinitialize_store_bridge(f.ledger,f.program,f.module,f.function_map(),graphs); }), "duplicate claimed ticket across source functions refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations[0].kind=Operation::Kind::Read; candidate(f,g); }), "write ID on a Read operation refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->eraseFromParent(); candidate(f,candidate_graph()); }), "erased emitted store refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->setOperand(0,llvm::ConstantInt::get(f.builder.getInt64Ty(),888)); candidate(f,candidate_graph()); }), "mutated LLVM store value refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); f.store->setMetadata("atto.ownership.field_write.source.v1",nullptr); candidate(f,candidate_graph()); }), "missing LLVM provenance metadata refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); llvm::Module foreign("foreign",f.context); std::map<const Function*,Graph> graphs; graphs.emplace(f.source_function,candidate_graph()); verify_candidate_reinitialize_store_bridge(f.ledger,f.program,foreign,f.function_map(),graphs); }), "foreign LLVM module refused", n);
        check(denied([]{ Fixture f; f.bind_primary(); std::map<const Function*,Graph> graphs; graphs.emplace(f.source_function,candidate_graph()); std::map<const Function*,const llvm::Function*> missing; verify_candidate_reinitialize_store_bridge(f.ledger,f.program,f.module,missing,graphs); }), "invalid declaration mapping refused", n);
        check([]{ Fixture f; f.bind_primary(); return safe_candidate(candidate(f,candidate_graph())); }(), "matching store ticket and moved-field CFG accepted", n);
        check([]{ Fixture f; f.bind_primary(); return candidate(f,candidate_graph()).checked_claims==1; }(), "bounded positive claim count exactly one", n);
        check([]{ Fixture f; f.bind_primary(); Graph g; g.entry.emplace("owner.x",PlaceState(oreslang::ownership::kOwned)); g.blocks.resize(1); g.blocks[0].operations.push_back({Operation::Kind::Read,"owner.x"}); auto a=candidate(f,g); return a.checked_claims==0 && !a.candidate_write_id_store_bound && !a.guest_verified; }(), "no source claim means no promotion", n);
        check([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(1,"owner.x",oreslang::ownership::kOwned); g.blocks[0].operations.insert(g.blocks[0].operations.begin(),{Operation::Kind::Move,"owner.x"}); g.blocks[0].operations.push_back({Operation::Kind::Read,"owner.x"}); return safe_candidate(candidate(f,g)); }(), "move then reinitialize then read accepted", n);
        check([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.blocks[0].operations.push_back({Operation::Kind::Read,"owner.x"}); return safe_candidate(candidate(f,g)); }(), "reinitialize followed by read accepted", n);
        check([]{ Fixture f; f.bind_primary(); auto& other=f.add(*f.source_function); f.bind(other,f.another()); auto g=candidate_graph(); g.entry.emplace("owner.y",PlaceState(oreslang::ownership::kMoved)); g.blocks[0].operations.emplace_back(Operation::Kind::Reinitialize,"owner.y",std::nullopt,2); auto a=candidate(f,g); return safe_candidate(a) && a.checked_claims==2; }(), "two real field stores and two corresponding IDs accepted", n);
        check([]{ Fixture f; f.bind_primary(); auto& other=f.add(*f.source_function); f.bind(other,f.another()); auto a=candidate(f,candidate_graph()); return safe_candidate(a) && !a.typed_mir_complete; }(), "two stores one reinitialization remains noncomplete", n);
        check([]{ Fixture f; f.bind_primary(); f.another(); return safe_candidate(candidate(f,candidate_graph())); }(), "ordinary untagged LLVM housekeeping store allowed", n);
        check([]{ Fixture f; f.bind_primary(); auto g=candidate_graph(); g.entry.emplace("other.y",PlaceState(oreslang::ownership::kOwned)); g.blocks[0].operations.emplace_back(Operation::Kind::Read,"other.y"); return safe_candidate(candidate(f,g)); }(), "unrelated owned field does not weaken exact place proof", n);
        check([]{ Fixture f; f.bind_primary(); auto a=candidate(f,candidate_graph()); return a.candidate_write_id_store_bound && !a.full_loan_use_coverage && !a.guest_verified; }(), "existing source store certificate not full NLL proof", n);
        check([]{ Fixture f; f.bind_primary(); auto& other=f.add(*f.source_function); f.bind(other,f.another()); auto g=candidate_graph(2,"owner.y"); return safe_candidate(candidate(f,g)); }(), "valid reinitialization from second store ticket", n);
        check([]{ Fixture f; f.bind_primary(); return !candidate(f,candidate_graph()).optimized_code_verified; }(), "checked candidate retains optimization proof false", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_reinitialize_write_id=0; direct_reinit(f); }), "source reinit ID reset after sealing rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_reinitialize_write_id=98; direct_reinit(f); }), "source reinit ID substituted after sealing rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_checked_field_reinitialization=false; direct_reinit(f); }), "source reinit claimed marker erased after sealing rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,false); f.primary->compiler_checked_field_reinitialization=true; direct_reinit(f); }), "ordinary source upgraded to reinit after sealing rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->kind=Statement::Kind::Call; direct_reinit(f); }), "non-field write forged as reinit rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_source_node_id=0; direct_reinit(f); }), "erased source node identity rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_source_path="f1/e0"; direct_reinit(f); }), "reparented source identity rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.source_function->compiler_source_function_id=9; direct_reinit(f); }), "relabelled source function identity rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.source_function->name="wrong"; direct_reinit(f); }), "renamed source function rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); ++f.program.compiler_source_unit_id; direct_reinit(f); }), "forged source unit epoch rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.source_function->statements.clear(); direct_reinit(f); }), "source node removed after emission rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->field="y"; direct_reinit(f); }), "field changed after emission rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->receiver="other"; direct_reinit(f); }), "source receiver changed after emission rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->offset=100; direct_reinit(f); }), "source byte offset changed after emission rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->eraseFromParent(); direct_reinit(f); }), "physical emitted StoreInst removed rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->setOperand(1,f.alternate); direct_reinit(f); }), "physical emitted store destination mutated rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->setOperand(0,llvm::ConstantInt::get(f.builder.getInt64Ty(),999)); direct_reinit(f); }), "physical emitted store value mutated rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->setMetadata("atto.ownership.reinitialize.source.v1",nullptr); direct_reinit(f); }), "reinit LLVM metadata removed rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"forged")})); direct_reinit(f); }), "reinit LLVM metadata malformed arity rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"wrong"),llvm::MDString::get(f.context,"wrong")})); direct_reinit(f); }), "reinit LLVM metadata noninteger rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* i64=f.builder.getInt64Ty(); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,900)),llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,1))})); direct_reinit(f); }), "reinit LLVM metadata forged unit ID rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* i64=f.builder.getInt64Ty(); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,f.program.compiler_source_unit_id)),llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64,900))})); direct_reinit(f); }), "reinit LLVM metadata forged write ID rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* i32=f.builder.getInt32Ty(); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32,1)),llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32,1))})); direct_reinit(f); }), "reinit LLVM metadata forged 32-bit integer rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* extra=f.another(); extra->setMetadata("atto.ownership.reinitialize.source.v1",f.store->getMetadata("atto.ownership.reinitialize.source.v1")); direct_reinit(f); }), "cloned reinitialization tag on unpaired store rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* extra=f.another(f.destination); extra->setMetadata("atto.ownership.reinitialize.source.v1",f.store->getMetadata("atto.ownership.reinitialize.source.v1")); direct_reinit(f); }), "reinit tag copied onto another untagged store rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,false); f.store->setMetadata("atto.ownership.reinitialize.source.v1",llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"forged")})); direct_reinit(f); }), "reinit LLVM tag on unmarked ordinary field write rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); f.primary->compiler_checked_field_reinitialization=false; f.primary->compiler_reinitialize_write_id=0; direct_reinit(f); }), "unmarked source after record mutation rejected", n);
        check(denied([]{ Fixture f; seal_direct_write(f,true); auto* extra=f.another(); extra->setMetadata("atto.ownership.field_write.source.v1",f.store->getMetadata("atto.ownership.field_write.source.v1")); direct_reinit(f); }), "extra emitted tagged store from unrelated source rejected", n);
        check([]{ Fixture f; seal_direct_write(f,true); return bounded_only(direct_reinit(f)); }(), "checker-flagged source bound to real LLVM store", n);
        check([]{ Fixture f; seal_direct_write(f,true); return f.primary->compiler_reinitialize_write_id==f.primary->compiler_source_node_id && bounded_only(direct_reinit(f)); }(), "source write ID corresponds to minted node ID", n);
        check([]{ Fixture f; seal_direct_write(f,true); auto* md=f.store->getMetadata("atto.ownership.reinitialize.source.v1"); return md && md->getNumOperands()==2 && bounded_only(direct_reinit(f)); }(), "source unit epoch persists in the reinit StoreInst", n);
        check([]{ Fixture f; seal_direct_write(f,true); return !direct_reinit(f).full_typed_mir_complete; }(), "source audit never grants full typed MIR completeness", n);
        check([]{ Fixture f; seal_direct_write(f,true); return !direct_reinit(f).full_loan_use_coverage; }(), "source audit never grants full LoanId completeness", n);
        check([]{ Fixture f; seal_direct_write(f,true); return !direct_reinit(f).optimized_code_verified; }(), "source audit never grants optimized code correctness", n);
        check([]{ Fixture f; seal_direct_write(f,true); return !direct_reinit(f).guest_verified; }(), "source audit never grants guest correctness", n);
        check([]{ Fixture f; seal_direct_write(f,false); auto a=direct_reinit(f); return a.checked_moved_field_writes==0 && !a.bounded_source_checked_stores_bound && !f.store->getMetadata("atto.ownership.reinitialize.source.v1"); }(), "ordinary field write gets no reinitialization metadata", n);
        check([]{ Fixture f; seal_direct_write(f,false); return f.primary->compiler_source_node_id!=0 && f.primary->compiler_reinitialize_write_id==0 && !direct_reinit(f).guest_verified; }(), "ordinary write receives source identity but not write ID", n);
        check([]{ Fixture f; seal_direct_write(f,true); (void)f.another(); return bounded_only(direct_reinit(f)); }(), "untagged housekeeping stores allowed with verified reinit", n);
        check([]{ Fixture f; seal_direct_write(f,true); auto* extra=f.another(f.alternate); return extra && bounded_only(direct_reinit(f)); }(), "ordinary unrelated LLVM housekeeping store with reinit allowed", n);
        check([]{ Fixture f; seal_direct_write(f,true); auto a=f.ledger.verify_source_coverage(f.program,f.module,f.function_map()); return a.immutable_source_identity_bound && !a.ownership_complete && bounded_only(direct_reinit(f)); }(), "verified source ledger and reinit proof remain separate", n);
        if(n != 154) throw std::runtime_error("wrong test inventory");
        std::cout<<"PASS 154/154 source FieldWrite + reinit candidate and checked-store cases "
                    "(REAL LLVM IR HOST TEST; NOT NLL/GUEST VERIFIED)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL "<<n<<": "<<error.what()<<'\n';
        return 1;
    }
}
