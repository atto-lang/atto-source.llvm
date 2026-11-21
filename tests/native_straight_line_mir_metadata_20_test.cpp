#include "oreslang/types/StraightLineSourceMir.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vector>

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
    std::size_t n = 0;
    try {
        using Mutation = std::function<void(Fixture&)>;
        const std::vector<std::pair<const char*, Mutation>> rejected = {
            {"dotted scalar owner", [](Fixture& f){ f.former().name="pair.left"; }},
            {"unresolved local type alias", [](Fixture& f){ f.former().pending_type_alias="OpaqueScalar"; }},
            {"unresolved return alias", [](Fixture& f){ f.function().pending_return_type_alias="OpaqueScalar"; }},
            {"linker internal function", [](Fixture& f){ f.function().native_internal=true; }},
            {"String-resolved result", [](Fixture& f){ f.function().result->resolved_type=oreslang::ScalarType::String; }},
            {"String array hint", [](Fixture& f){ f.former().array_element_type=oreslang::ScalarType::String; }},
            {"excessive source bytes", [](Fixture& f){ f.former().name.assign(1024*1024+1,'x'); }},
            {"oversize unrelated class", [](Fixture& f){ ClassDecl c; c.name="Unused"; c.fields.resize(1025); f.program.classes.push_back(std::move(c)); }},
        };
        for (const auto& entry : rejected) {
            Fixture before(true, false);
            entry.second(before);
            before.seal();
            check(!before.trace().has_value(), entry.first, n);
            check(denied([&]{
                Fixture after;
                entry.second(after);
                (void)after.trace();
            }), entry.first, n);
        }
        const std::vector<std::pair<const char*, Mutation>> metadata = {
            {"class constructor export identity", [](Fixture& f){ f.program.classes.front().constructor_exported=true; }},
            {"function export identity", [](Fixture& f){ f.function().exported=true; }},
        };
        for (const auto& entry : metadata) {
            Fixture before(true, false);
            entry.second(before);
            before.seal();
            check(before.trace().has_value(), entry.first, n);
            check(denied([&]{
                Fixture after;
                entry.second(after);
                (void)after.trace();
            }), entry.first, n);
        }
        if (n != 20) throw std::runtime_error("metadata count changed");
        std::cout << "PASS 20/20 metadata and source-budget HOST cases"
                     " (NOT LLVM GUEST PROOF)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED " << n << ": " << error.what() << '\n';
        return 1;
    }
}
