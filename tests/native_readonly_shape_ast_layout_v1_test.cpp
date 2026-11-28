// Real native Parser.hpp Program / NativeClassLayouts host-level proof.
// Neither parse_program nor LLVM IR lowering is invoked by this test.
#include "../src/main/cpp/oreslang/types/ReadonlyNativeClassShapeBridgeV1.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang;
using namespace oreslang::frontend::shape_v1;

static ClassField integer_field(std::string name, std::int64_t value,
                                bool is_public = true, bool fixed = true) {
    ClassField f;
    f.name = std::move(name);
    f.initial = value;
    f.public_member = is_public;
    f.final_field = fixed;
    f.scalar_type = ScalarType::I64;
    return f;
}
static ClassDecl make_class(std::string name, std::vector<ClassField> fields) {
    ClassDecl c;
    c.name = std::move(name);
    c.fields = std::move(fields);
    return c;
}
static void require(bool b, const char* label) {
    if (!b) throw std::runtime_error(std::string("native AST witness: ") + label);
}
template <typename F> static void rejected(F action, const char* label) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected native AST rejection: ") + label);
}
static plan checked(const Program& ast, const std::string& name, std::uint32_t id) {
    return prove_native_class_layout(ast, name, id, 8, 11,
                                     {{"x", scalar::i64}});
}
int main() {
    Program p;
    {
        std::vector<ClassField> a;
        a.push_back(integer_field("x",42));
        p.classes.push_back(make_class("A",std::move(a)));
    }
    {
        std::vector<ClassField> b;
        b.push_back(integer_field("padding",10));
        b.push_back(integer_field("x",42));
        p.classes.push_back(make_class("B",std::move(b)));
    }
    const auto a = checked(p,"A",10);
    const auto b = checked(p,"B",11);
    require(a.get_field("x").source_slot==0,"A.x actual source slot");
    require(b.get_field("x").source_slot==1,"B.x actual source slot");
    require(a.source_nominal_id!=b.source_nominal_id,"distinct verified nominal source IDs");
    require(p.classes[0].fields.at(a.get_field("x").source_slot).initial
            + p.classes[1].fields.at(b.get_field("x").source_slot).initial == 84,
            "cross-layout 84 oracle from actual compiler AST");
    p.classes[0].fields[0].public_member=false;
    rejected([&]{ (void)checked(p,"A",10); },"private x");
    p.classes[0].fields[0].public_member=true;
    p.classes[0].fields[0].final_field=false;
    rejected([&]{ (void)checked(p,"A",10); },"mutable x");
    p.classes[0].fields[0].final_field=true;
    p.classes[0].fields[0].scalar_type=ScalarType::Bool;
    rejected([&]{ (void)checked(p,"A",10); },"non-i64 x");
    p.classes[0].fields[0].scalar_type=ScalarType::I64;
    rejected([&] { (void)prove_native_class_layout(
        p,"unknown",13,8,11,{{"x",scalar::i64}}); },"unknown class");
    rejected([&] { (void)prove_native_class_layout(
        p,"A",10,0,11,{{"x",scalar::i64}}); },"stale epoch");
    rejected([&] { (void)prove_native_class_layout(
        p,"A",10,8,0,{{"x",scalar::i64}}); },"missing loan");
    std::cout << "LLVM NATIVE AST LAYOUT WITNESS PASS; source slots 0+1 oracle 84; guest NOT_EXECUTED\n";
}
