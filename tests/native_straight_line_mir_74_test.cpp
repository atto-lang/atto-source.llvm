#include "oreslang/types/StraightLineSourceMir.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::ClassDecl;
using oreslang::ClassField;
using oreslang::Expression;
using oreslang::Function;
using oreslang::LocalBinding;
using oreslang::Program;
using oreslang::Statement;
using oreslang::native_mir::SourcePhase;
using oreslang::native_mir::build_checked_straight_line_trace;
using Kind = Expression::Kind;
using Op = oreslang::ownership::Operation::Kind;

std::unique_ptr<Expression> expr(Kind kind, std::string name = "") {
    auto p = std::make_unique<Expression>();
    p->kind = kind;
    p->name = std::move(name);
    return p;
}
std::unique_ptr<Expression> field(const std::string& root,
        const std::string& name) {
    auto p=expr(Kind::Field, name);
    p->left=expr(Kind::Variable, root);
    return p;
}
std::unique_ptr<Expression> take(std::unique_ptr<Expression> source) {
    auto p=expr(Kind::Take);
    p->left=std::move(source);
    return p;
}
std::unique_ptr<Expression> sum(
        std::unique_ptr<Expression> lhs,
        std::unique_ptr<Expression> rhs) {
    auto p=expr(Kind::Add);
    p->left=std::move(lhs);
    p->right=std::move(rhs);
    return p;
}
struct Fixture {
    Program program;
    explicit Fixture(bool checked=true, bool sealed=true) {
        ClassDecl cls;
        cls.name="Pair";
        for (const auto& name : {"left", "right"}) {
            ClassField f;
            f.name=name;
            f.public_member=true;
            f.initializer_expression=expr(Kind::Integer);
            cls.fields.push_back(std::move(f));
        }
        program.classes.push_back(std::move(cls));
        Function fn;
        fn.name="main";
        LocalBinding pair;
        pair.name="pair";
        pair.class_name="Pair";
        pair.new_class=true;
        pair.owner_mutable=true;
        fn.locals.push_back(std::move(pair));
        LocalBinding former;
        former.name="former";
        former.initializer=take(field("pair","left"));
        fn.locals.push_back(std::move(former));
        Statement write;
        write.kind=Statement::Kind::FieldWrite;
        write.receiver="pair";
        write.field="left";
        write.expression=expr(Kind::Variable,"former");
        write.offset=40;
        write.compiler_checked_field_reinitialization=checked;
        fn.statements.push_back(std::move(write));
        if (checked) fn.compiler_checked_reinitialize_operations.push_back(
            {0,"pair.left",40,0});
        fn.result=sum(field("pair","left"),field("pair","right"));
        program.functions.push_back(std::move(fn));
        if (sealed) seal();
    }
    Function& function() {return program.functions.front();}
    Statement& write() {return function().statements.front();}
    LocalBinding& former() {return function().locals[1];}
    void seal() {oreslang::source_identity::seal_validated_program(program);}
    auto trace() const {
        return build_checked_straight_line_trace(
            program,program.functions.front());
    }
};
void add_scalar_write(Fixture& f) {
    Statement write;
    write.kind=Statement::Kind::ScalarWrite;
    write.receiver="former";
    write.expression=expr(Kind::Integer);
    write.offset=61;
    f.function().statements.push_back(std::move(write));
}
bool denied(const std::function<void()>& run) {
    try {run();}
    catch(const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:")!=
            std::string::npos;
    }
    return false;
}
void check(bool ok, const char* label, std::size_t& n) {
    ++n;
    if(!ok) throw std::runtime_error(
        "straight-line MIR case "+std::to_string(n)+": "+label);
}
int main() {
    std::size_t n=0;
    try {
        check([]{ Fixture f; auto t=f.trace(); return t.has_value() && t->supported_source_effects_complete; }(), "compiler-created fully modeled trace", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->graph.blocks.size()==1 && t->graph.blocks[0].successors.empty(); }(), "single linear CFG block", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps.size()==8 && t->graph.blocks[0].operations.size()==8; }(), "eight source ownership operations", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[0].operation.kind==Op::Initialize && t->steps[1].operation.kind==Op::Initialize; }(), "class constructor fields initialized before move", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[2].operation.kind==Op::Move && t->steps[2].operation.place=="pair.left"; }(), "source take moves the owned left projection", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[3].operation.kind==Op::Initialize && t->steps[3].operation.place=="former"; }(), "taken scalar former initialized after move", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[4].operation.kind==Op::Read && t->steps[4].operation.place=="former"; }(), "returning RHS former reads it before reinit", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[5].operation.kind==Op::Reinitialize && t->steps[5].operation.place=="pair.left" && t->steps[5].operation.write_id==f.write().compiler_reinitialize_write_id; }(), "source WriteId reinitializes only definitely moved field", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[5].statement_node_id==f.write().compiler_source_node_id && t->steps[5].phase==SourcePhase::Statement; }(), "source write site records exact AST node", n);
        check([]{ Fixture f; auto t=f.trace(); return t && t->steps[6].operation.kind==Op::Read && t->steps[7].operation.kind==Op::Read && t->steps[6].phase==SourcePhase::Result; }(), "final result reads exactly two fields", n);
        check([]{ Fixture f; auto t=f.trace(); return t && !t->full_loan_use_coverage && !t->optimized_code_verified && !t->guest_verified; }(), "complete restricted source does not prove native SSA or guest", n);
        check([]{ Fixture f(false,false); f.former().initializer=expr(Kind::Integer); f.seal(); auto t=f.trace(); return t && t->steps[4].operation.kind==Op::WriteOwned && !t->steps[4].operation.write_id; }(), "ordinary owned field write modeled separately", n);
        check(denied([]{ Fixture f(true,false); f.former().initializer=expr(Kind::Integer); f.seal(); (void)f.trace(); }), "checked reinit without source move rejected", n);
        check(denied([]{ Fixture f(true,false); f.write().expression=field("pair","left"); f.seal(); (void)f.trace(); }), "moved field cannot be read in own assignment RHS", n);
        check(denied([]{ Fixture f(false); (void)f.trace(); }), "ordinary assignment cannot silently restore moved place", n);
        check(denied([]{ Fixture f(true,false); f.write().field="right"; f.function().compiler_checked_reinitialize_operations.front().canonical_place="pair.right"; f.seal(); (void)f.trace(); }), "wrong field reinit after take rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_checked_reinitialize_operations.clear(); (void)f.trace(); }), "checker reinit source inventory erased rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_checked_reinitialize_operations.push_back(f.function().compiler_checked_reinitialize_operations.front()); (void)f.trace(); }), "checker reinit source inventory duplicated rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_checked_reinitialize_operations.front().canonical_place="pair.right"; (void)f.trace(); }), "checker reinit source inventory wrong place rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_checked_reinitialize_operations.front().statement_index=9; (void)f.trace(); }), "checker reinit source inventory wrong ordinal rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_checked_reinitialize_operations.front().sealed_write_id=999; (void)f.trace(); }), "checker reinit sealed WriteId substitution rejected", n);
        check(denied([]{ Fixture f; f.write().compiler_checked_field_reinitialization=false; (void)f.trace(); }), "source reinit marker cleared after sealing rejected", n);
        check(denied([]{ Fixture f; f.write().compiler_source_node_id=19; (void)f.trace(); }), "new source node identity after sealing rejected", n);
        check(denied([]{ oreslang::ownership::Graph g; g.entry.emplace("pair.left", oreslang::ownership::PlaceState(oreslang::ownership::kOwned,1,false,{0})); g.blocks.resize(1); g.blocks[0].operations.emplace_back(Op::WriteOwned,"pair.left"); (void)oreslang::ownership::verify(g); }), "ordinary field write cannot overlap shared loan", n);
        check([]{ Fixture f(true,false); f.function().result=expr(Kind::Call,"dynamic"); f.seal(); return !f.trace().has_value(); }(), "unsupported function call in result returns unproved", n);
        check([]{ Fixture f(true,false); f.function().result=expr(Kind::Conditional); f.seal(); return !f.trace().has_value(); }(), "unsupported conditional return returns unproved", n);
        check([]{ Fixture f(true,false); f.function().result=expr(Kind::Borrow); f.seal(); return !f.trace().has_value(); }(), "unsupported borrow expression returns unproved", n);
        check([]{ Fixture f(true,false); f.function().actor_stops.push_back("worker"); f.seal(); return !f.trace().has_value(); }(), "unsupported actor-owned operations return unproved", n);
        check([]{ Fixture f(true,false); oreslang::Parameter arg; arg.name="param"; f.function().parameters.push_back(std::move(arg)); f.seal(); return !f.trace().has_value(); }(), "unsupported function parameters return unproved", n);
        check([]{ Fixture f(true,false); f.function().early_returns.emplace_back(); f.seal(); return !f.trace().has_value(); }(), "unsupported return guard returns unproved", n);
        check([]{ Fixture f(true,false); f.program.classes.front().fields.front().int_array=true; f.seal(); Fixture g(true,false); g.program.classes.front().fields.front().initializer_expression.reset(); g.seal(); return !f.trace().has_value() && !g.trace().has_value(); }(), "array field or absent constructor initializer returns unproved", n);
        check([]{ Fixture f(true,false); f.function().result.reset(); f.seal(); return !f.trace().has_value(); }(), "unsupported missing source return returns unproved", n);
        check(denied([]{ Fixture f; ++f.program.classes.front().fields.front().initializer_expression->integer; (void)f.trace(); }), "post-seal class initializer value rewrite denied", n);
        check(denied([]{ Fixture f; std::swap(f.program.classes.front().fields[0],f.program.classes.front().fields[1]); (void)f.trace(); }), "post-seal constructor field declaration reordered denied", n);
        check(denied([]{ Fixture f; f.program.classes.front().fields.front().scalar_type=oreslang::ScalarType::String; (void)f.trace(); }), "post-seal field semantic type changed denied", n);
        check(denied([]{ Fixture f; f.program.classes.front().fields.front().final_field=true; (void)f.trace(); }), "post-seal class field mutability rewritten denied", n);
        check(denied([]{ Fixture f; f.former().initializer=expr(Kind::Integer); (void)f.trace(); }), "post-seal local init rewired denied", n);
        check(denied([]{ Fixture f; f.write().expression=expr(Kind::Integer); (void)f.trace(); }), "post-seal source statement RHS rewritten denied", n);
        check(denied([]{ Fixture f; f.function().result=expr(Kind::Integer); (void)f.trace(); }), "post-seal return expression rewritten denied", n);
        check(denied([]{ Fixture f; f.former().name="surprise"; (void)f.trace(); }), "post-seal lexical name changed denied", n);
        check(denied([]{ Fixture f; f.former().assignable=true; (void)f.trace(); }), "post-seal lexical mutability changed denied", n);
        check(denied([]{ Fixture f; f.function().locals.front().owner_mutable=false; (void)f.trace(); }), "post-seal class mutability changed denied", n);
        check(denied([]{ Fixture f; LocalBinding local; local.name="new_slot"; local.initializer=expr(Kind::Integer); f.function().locals.push_back(std::move(local)); (void)f.trace(); }), "post-seal extra local owner denied", n);
        check(denied([]{ Fixture f; ClassDecl cls; cls.name="Foreign"; f.program.classes.push_back(std::move(cls)); (void)f.trace(); }), "post-seal extra class declaration denied", n);
        check(denied([]{ Fixture f; f.function().compiler_straight_line_effect_fingerprint=0; (void)f.trace(); }), "source effect shape fingerprint removed denied", n);
        check(denied([]{ Fixture f; f.function().compiler_straight_line_effect_fingerprint=42; (void)f.trace(); }), "source effect shape fingerprint substituted denied", n);
        check(denied([]{ Fixture f(true,false); f.function().compiler_straight_line_effect_fingerprint=123; f.seal(); }), "source effect fingerprint preassigned before seal denied", n);
        check(denied([]{ Fixture f; f.function().result=expr(Kind::Integer); oreslang::source_identity::prune_and_reseal_validated_functions(f.program,{1}); }), "source effect mutation before trusted tree-shake rejected", n);
        check([]{ Fixture f(true,false); f.program.classes.front().fields.front().scalar_type=oreslang::ScalarType::String; f.seal(); return !f.trace().has_value(); }(), "String class field handle not complete", n);
        check([]{ Fixture f(true,false); f.former().scalar_type=oreslang::ScalarType::String; f.seal(); return !f.trace().has_value(); }(), "String scalar local handle not complete", n);
        check([]{ Fixture f(true,false); f.function().return_type=oreslang::ScalarType::String; f.seal(); return !f.trace().has_value(); }(), "String return handle not complete", n);
        check([]{ Fixture f(true,false); f.program.classes.front().fields.front().final_field=true; f.seal(); return !f.trace().has_value(); }(), "final class field excluded from subset", n);
        check([]{ Fixture f(true,false); f.function().locals.front().owner_mutable=false; f.seal(); return !f.trace().has_value(); }(), "immutable class referent cannot carry direct field write", n);
        check([]{ Fixture f(true,false); add_scalar_write(f); f.seal(); return !f.trace().has_value(); }(), "non-assignable scalar lexical slot cannot be written", n);
        check([]{ Fixture f(true,false); f.former().assignable=true; add_scalar_write(f); f.seal(); auto t=f.trace(); return t && t->supported_source_effects_complete && t->steps[6].operation.kind==Op::WriteOwned && t->steps[6].operation.place=="former"; }(), "assignable scalar slot emits WriteOwned", n);
        check([]{ Fixture f; auto previous=f.function().compiler_straight_line_effect_fingerprint; oreslang::source_identity::prune_and_reseal_validated_functions(f.program,{1}); auto t=f.trace(); return t && previous==f.function().compiler_straight_line_effect_fingerprint && t->supported_source_effects_complete; }(), "trusted tree shake reissues source shape epoch", n);
        check([]{ Fixture f; auto a=f.function().compiler_straight_line_effect_fingerprint; oreslang::source_identity::validate_program_identity(f.program); return a!=0 && a==f.function().compiler_straight_line_effect_fingerprint && f.trace().has_value(); }(), "same source shape remains stable across validation", n);
        check([]{ Fixture f; auto t=f.trace(); return t && !t->full_loan_use_coverage && !t->guest_verified && !t->optimized_code_verified; }(), "scoped trace remains nonpromoting after shape fence", n);
        check(denied([]{ Fixture f; f.function().generic_parameters.emplace_back(); (void)f.trace(); }), "post-seal generics added denied", n);
        check(denied([]{ Fixture f; f.function().ready_future_name="ready"; (void)f.trace(); }), "post-seal actor ready token altered denied", n);
        check(denied([]{ Fixture f; f.function().return_sum="Option"; (void)f.trace(); }), "post-seal sum return type altered denied", n);
        check(denied([]{ Fixture f; f.function().has_declared_effects=true; (void)f.trace(); }), "post-seal effect contract toggle denied", n);
        check([]{ Fixture f(true,false); f.function().generic_parameters.emplace_back(); f.seal(); return !f.trace().has_value(); }(), "generic functions outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.function().ready_future_name="ready"; f.seal(); return !f.trace().has_value(); }(), "ready Future token outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.function().ready_future_actor="worker"; f.seal(); return !f.trace().has_value(); }(), "ready Future actor outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.function().has_declared_effects=true; f.seal(); return !f.trace().has_value(); }(), "declared effect contract outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.function().return_sum="Option"; f.seal(); return !f.trace().has_value(); }(), "sum return outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.program.classes.front().explicit_constructor=true; f.seal(); return !f.trace().has_value(); }(), "explicit class constructor outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.program.classes.front().constructor_parameters.push_back("left"); f.seal(); return !f.trace().has_value(); }(), "class constructor parameters outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.former().sum_type="Option"; f.seal(); return !f.trace().has_value(); }(), "local sum type outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.former().array_record_class="Record"; f.seal(); return !f.trace().has_value(); }(), "local record-array hint outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.former().move_source="pair"; f.seal(); return !f.trace().has_value(); }(), "local class-move origin outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.program.classes.front().fields.front().sum_type="Option"; f.seal(); return !f.trace().has_value(); }(), "sum-typed class field outside restricted MIR", n);
        check([]{ Fixture f(true,false); f.program.classes.front().fields.front().array_record_class="Record"; f.seal(); return !f.trace().has_value(); }(), "record-array class field outside restricted MIR", n);
        if(n!=74) throw std::runtime_error("source MIR test count changed");
        std::cout<<"PASS 74/74 compiler-source straight-line MIR and shape-fence host cases "
                    "(NOT LLVM GUEST PROOF)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAILED "<<n<<": "<<error.what()<<'\n';
        return 1;
    }
}
