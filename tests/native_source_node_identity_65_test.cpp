#include "oreslang/types/SourceNodeIdentity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::Program;
using oreslang::Function;
using oreslang::Statement;
using oreslang::source_identity::seal_validated_program;
using oreslang::source_identity::validate_program_identity;

struct Fixture {
    Program program;
    Fixture() {
        program.functions.reserve(8);
        auto& function = program.functions.emplace_back();
        function.name = "main";
        function.statements.reserve(20);
        auto& write = function.statements.emplace_back();
        write.kind = Statement::Kind::FieldWrite;
        write.receiver = "owner";
        write.field = "a";
        write.offset = 31;
    }
    Function& function() { return program.functions.front(); }
    Statement& first() { return function().statements.front(); }
    Statement& add(const std::string& field="b") {
        auto& s = function().statements.emplace_back();
        s.kind = Statement::Kind::FieldWrite;
        s.receiver = "owner";
        s.field = field;
        s.offset = 40;
        return s;
    }
    Statement& branch(bool yes=true) {
        auto& children = yes ? first().then_statements : first().else_statements;
        auto& s=children.emplace_back();
        s.kind=Statement::Kind::FieldWrite;
        s.receiver="owner"; s.field="b"; s.offset=42;
        return s;
    }
    Statement& caught() {
        auto& s=function().caught_throw_defers.emplace_back();
        s.kind=Statement::Kind::FieldWrite;
        s.receiver="owner"; s.field="cleanup"; s.offset=55;
        return s;
    }
    Function& second() {
        auto& s=program.functions.emplace_back();
        s.name="secondary";
        return s;
    }
    void seal() {seal_validated_program(program);}
    void validate() const {validate_program_identity(program);}
};

bool denied(const std::function<void()>& fn) {
    try {fn();}
    catch(const std::runtime_error& err) {
        return std::string(err.what()).find("ownership violation:")==0;
    }
    return false;
}
void check(bool pass, const char* name, std::size_t& n) {
    ++n;
    if(!pass)throw std::runtime_error(std::string("case ")+
        std::to_string(n)+": "+name);
}
int main() {
    std::size_t n=0;
    try {
        check(denied([]{ Fixture f; f.validate(); }), "unsealed source identity fails closed", n);
        check(denied([]{ Fixture f; f.seal(); f.seal(); }), "second seal of same Program rejected", n);
        check(denied([]{ Fixture f; f.program.compiler_source_unit_id=8; f.seal(); }), "user-preassigned source unit identity rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_source_function_id=42; f.seal(); }), "user-preassigned source function identity rejected", n);
        check(denied([]{ Fixture f; f.function().compiler_source_sealed_name="main"; f.seal(); }), "user-preassigned source function name rejected", n);
        check(denied([]{ Fixture f; f.first().compiler_source_node_id=10; f.seal(); }), "user-preassigned source node id rejected", n);
        check(denied([]{ Fixture f; f.first().compiler_source_path="f1/s0"; f.seal(); }), "user-preassigned structural source path rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.program.compiler_source_unit_id=0; f.validate(); }), "cleared compilation identity rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.program.compiler_source_node_count=0; f.validate(); }), "cleared node count rejected", n);
        check(denied([]{ Fixture f; f.seal(); ++f.program.compiler_source_node_count; f.validate(); }), "increased node count rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.program.compiler_source_function_count=0; f.validate(); }), "cleared function count rejected", n);
        check(denied([]{ Fixture f; f.seal(); ++f.program.compiler_source_function_count; f.validate(); }), "increased function count rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().statements.clear(); f.validate(); }), "removed final source statement rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.program.functions.clear(); f.validate(); }), "removed final source function rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.add("c"); f.validate(); }), "added source statement after seal rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.second(); f.validate(); }), "added source function after seal rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().statements.clear(); auto& s=f.add("a"); s.offset=31; f.validate(); }), "replaced source statement at same vector slot rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.program.functions.clear(); f.program.functions.emplace_back().name="main"; f.validate(); }), "replaced source function at same slot rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.first().compiler_source_node_id=0; f.validate(); }), "zeroed existing source node id rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.first().compiler_source_node_id=64; f.validate(); }), "changed existing source node id rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().compiler_source_function_id=0; f.validate(); }), "zeroed source function id rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().compiler_source_function_id=3; f.validate(); }), "changed source function id rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().name="renamed"; f.validate(); }), "renamed source function rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.function().compiler_source_sealed_name="forged"; f.validate(); }), "corrupted sealed source name rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.first().compiler_source_path.clear(); f.validate(); }), "removed structural path rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.first().compiler_source_path="f2/s0"; f.validate(); }), "moved structural path to another function rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.first().compiler_source_path="f1/s1"; f.validate(); }), "changed root statement index path rejected", n);
        check(denied([]{ Fixture f; f.add(); f.seal(); f.function().statements.pop_back(); f.validate(); }), "deleted last node of two rejected", n);
        check(denied([]{ Fixture f; f.add(); f.seal(); f.function().statements.erase(f.function().statements.begin()); f.validate(); }), "erased first node of two rejected", n);
        check(denied([]{ Fixture f; f.add(); f.seal(); std::swap(f.function().statements[0],f.function().statements[1]); f.validate(); }), "swapped two sources without remint rejected", n);
        check(denied([]{ Fixture f; f.add(); f.seal(); f.function().statements[1].compiler_source_node_id=f.first().compiler_source_node_id; f.validate(); }), "duplicated ID on second source rejected", n);
        check(denied([]{ Fixture f; f.add(); f.seal(); f.function().statements[1].compiler_source_path=f.first().compiler_source_path; f.validate(); }), "duplicated route on second source rejected", n);
        check(denied([]{ Fixture f; f.branch(); f.seal(); auto s=std::move(f.first().then_statements.front()); f.first().then_statements.clear(); f.first().else_statements.push_back(std::move(s)); f.validate(); }), "moved then child to else branch rejected", n);
        check(denied([]{ Fixture f; f.branch(false); f.seal(); auto s=std::move(f.first().else_statements.front()); f.first().else_statements.clear(); f.first().then_statements.push_back(std::move(s)); f.validate(); }), "moved else child to then branch rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.branch(); f.validate(); }), "appended nested child after seal rejected", n);
        check(denied([]{ Fixture f; f.branch(); f.seal(); f.first().then_statements.clear(); f.validate(); }), "deleted nested child after seal rejected", n);
        check(denied([]{ Fixture f; f.caught(); f.seal(); auto s=std::move(f.function().caught_throw_defers.front()); f.function().caught_throw_defers.clear(); f.function().statements.push_back(std::move(s)); f.validate(); }), "moved caught cleanup into normal statements rejected", n);
        check(denied([]{ Fixture f; f.caught(); f.seal(); f.function().caught_throw_defers.clear(); f.validate(); }), "deleted caught cleanup after seal rejected", n);
        check(denied([]{ Fixture f; f.seal(); f.caught(); f.validate(); }), "appended caught cleanup after seal rejected", n);
        check(denied([]{ Fixture f; f.second(); f.seal(); f.program.functions[1].name="altered"; f.validate(); }), "renamed second function after seal rejected", n);
        check(denied([]{ Fixture f; f.second(); f.seal(); f.program.functions[1].compiler_source_function_id=1; f.validate(); }), "duplicate second function ID rejected", n);
        check(denied([]{ Fixture f; f.second(); f.seal(); f.program.functions.pop_back(); f.validate(); }), "deleted second function after seal rejected", n);
        check(denied([]{ Fixture f; f.second(); f.seal(); std::swap(f.program.functions[0],f.program.functions[1]); f.validate(); }), "swapped two sealed functions rejected", n);
        check(denied([]{ Fixture f; f.program.compiler_source_function_count=1; f.validate(); }), "unvalidated source cannot be accepted through forged function count", n);
        check(denied([]{ Fixture f; f.branch().compiler_source_node_id=5; f.seal(); }), "partially presealed nested node rejected", n);
        check([]{ Fixture f; f.seal(); f.validate(); return true; }(), "single source node sealed and validated", n);
        check([]{ Fixture f; f.seal(); return f.function().compiler_source_function_id==1; }(), "single function ID compiler assigned", n);
        check([]{ Fixture f; f.seal(); return f.first().compiler_source_node_id==1; }(), "first source node ID compiler assigned", n);
        check([]{ Fixture f; f.seal(); return f.first().compiler_source_path=="f1/s0"; }(), "first source route source-derived", n);
        check([]{ Fixture f; f.seal(); return f.program.compiler_source_node_count==1 && f.program.compiler_source_function_count==1; }(), "source count sealed", n);
        check([]{ Fixture f; f.seal(); return f.program.compiler_source_unit_id>0; }(), "positive compilation-unit epoch", n);
        check([]{ Fixture f; Fixture g; f.seal(); g.seal(); return f.program.compiler_source_unit_id!=g.program.compiler_source_unit_id; }(), "second valid program receives a distinct epoch", n);
        check([]{ Fixture f; f.add(); f.seal(); f.validate(); return f.function().statements[1].compiler_source_node_id==2; }(), "two direct statements have distinct IDs", n);
        check([]{ Fixture f; f.add(); f.seal(); f.validate(); return f.function().statements[1].compiler_source_path=="f1/s1"; }(), "two direct statements have distinct routes", n);
        check([]{ Fixture f; f.branch(); f.seal(); f.validate(); return f.first().then_statements[0].compiler_source_path=="f1/s0/t0"; }(), "then child receives structural route", n);
        check([]{ Fixture f; f.branch(false); f.seal(); f.validate(); return f.first().else_statements[0].compiler_source_path=="f1/s0/e0"; }(), "else child receives structural route", n);
        check([]{ Fixture f; f.branch(); f.branch(false); f.seal(); f.validate(); return f.first().then_statements[0].compiler_source_node_id==2 && f.first().else_statements[0].compiler_source_node_id==3; }(), "two children have independent structural routes", n);
        check([]{ Fixture f; f.caught(); f.seal(); f.validate(); return f.function().caught_throw_defers[0].compiler_source_path=="f1/c0"; }(), "caught cleanup receives structural route", n);
        check([]{ Fixture f; f.second(); f.seal(); f.validate(); return f.program.functions[1].compiler_source_function_id==2; }(), "second Function has independent identity", n);
        check([]{ Fixture f; f.second(); f.seal(); f.validate(); return f.program.functions[1].compiler_source_sealed_name=="secondary"; }(), "second Function name preserved", n);
        check([]{ Fixture f; f.add(); f.seal(); Program moved=std::move(f.program); validate_program_identity(moved); return moved.compiler_source_node_count==2; }(), "moved Program retains verified identity", n);
        check([]{ Fixture f; Program empty; seal_validated_program(empty); validate_program_identity(empty); return empty.compiler_source_node_count==0; }(), "empty Program can be sealed without positive node proof", n);
        check([]{ Fixture f; f.program.functions.clear(); f.program.functions.emplace_back().name="empty"; f.seal(); f.validate(); return f.program.compiler_source_node_count==0; }(), "empty Function can be sealed without positive node proof", n);
        check([]{ Fixture f; auto& s=f.add(); s.kind=Statement::Kind::Call; f.seal(); f.validate(); return s.compiler_source_node_id==2; }(), "mixed non-field Statement gets immutable ID", n);
        check([]{ Fixture f; f.branch(); f.add("z"); f.seal(); f.validate(); return f.function().statements[1].compiler_source_node_id==3; }(), "nested children and root siblings retain order", n);
        if(n!=65) throw std::runtime_error("wrong source identity test inventory");
        std::cout<<"PASS 65/65 source identity cases (HOST ONLY; NOT LLVM NLL)\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAILED "<<n<<": "<<error.what()<<'\n';
        return 1;
    }
}
