#include "oreslang/backend/OwnershipSourceLoweringV1.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace oreslang;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return false; } } while (0)

static std::unique_ptr<Expression> expr(Expression::Kind kind) {
    auto e = std::make_unique<Expression>();
    e->kind = kind;
    return e;
}
static std::unique_ptr<Expression> variable(const std::string& name) {
    auto e=expr(Expression::Kind::Variable); e->name=name; return e;
}
static std::unique_ptr<Expression> field(const std::string& owner,const std::string& name) {
    auto e=expr(Expression::Kind::Field); e->left=variable(owner); e->name=name; return e;
}
static Program valid_source_program() {
    Program p;
    ClassDecl cls; cls.name="Pair";
    ClassField flag; flag.name="flag"; flag.public_member=true;
    flag.final_field=true; flag.initializer_expression=expr(Expression::Kind::Integer);
    flag.initializer_expression->integer=1;
    ClassField value; value.name="value"; value.public_member=true;
    value.final_field=true; value.initializer_expression=expr(Expression::Kind::Integer);
    value.initializer_expression->integer=42;
    cls.fields.push_back(std::move(flag)); cls.fields.push_back(std::move(value));
    p.classes.push_back(std::move(cls));

    Function fn; fn.name="main"; fn.return_type=ScalarType::I64;
    for (const char* name : {"first","second"}) {
        LocalBinding local; local.name=name; local.class_name="Pair"; local.new_class=true;
        fn.locals.push_back(std::move(local));
    }
    GuardedReturn guard;
    guard.condition=expr(Expression::Kind::Equal);
    guard.condition->left=field("first","flag");
    guard.condition->right=expr(Expression::Kind::Integer);
    guard.condition->right->integer=1;
    guard.result=field("first","value");
    fn.early_returns.push_back(std::move(guard));
    fn.result=field("second","value");
    p.functions.push_back(std::move(fn));
    return p;
}
static bool maps_resolved_source_to_two_exit_cfg() {
    auto p=valid_source_program();
    const auto result=OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
        p,p.functions.front(),p.classes.front());
    CHECK(result.normal_exits.size()==2);
    for (const auto& exit:result.normal_exits) {
        CHECK(exit.cleanup_reverse_order.size()==2);
        CHECK(exit.cleanup_reverse_order[0]==1);
        CHECK(exit.cleanup_reverse_order[1]==0);
    }
    CHECK(result.reachable_blocks==3);
    CHECK(result.analyzed_edges==2);
    return true;
}
static bool rejects_unowned_guard_receiver() {
    auto p=valid_source_program();
    p.functions.front().early_returns.front().condition->left=field("foreign","flag");
    try {
        (void)OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            p,p.functions.front(),p.classes.front());
    } catch(const std::runtime_error&) { return true; }
    return false;
}
static bool rejects_unknown_return_field() {
    auto p=valid_source_program();
    p.functions.front().result=field("second","missing");
    try {
        (void)OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            p,p.functions.front(),p.classes.front());
    } catch(const std::runtime_error&) { return true; }
    return false;
}
static bool rejects_unmodeled_statement_edges() {
    auto p=valid_source_program();
    Statement statement; statement.kind=Statement::Kind::Defer;
    p.functions.front().statements.push_back(std::move(statement));
    try {
        (void)OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            p,p.functions.front(),p.classes.front());
    } catch(const std::runtime_error&) { return true; }
    return false;
}
static bool rejects_hidden_program_roots() {
    auto p=valid_source_program();
    p.actors.emplace_back();
    try {
        (void)OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            p,p.functions.front(),p.classes.front());
    } catch(const std::runtime_error&) { return true; }
    return false;
}
static bool rejects_mutable_or_escaping_owner() {
    auto p=valid_source_program();
    p.functions.front().locals.front().owner_mutable=true;
    try {
        (void)OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            p,p.functions.front(),p.classes.front());
    } catch(const std::runtime_error&) { return true; }
    return false;
}
int main() {
    const bool ok = maps_resolved_source_to_two_exit_cfg() &&
        rejects_unowned_guard_receiver() && rejects_unknown_return_field() &&
        rejects_unmodeled_statement_edges() && rejects_hidden_program_roots() &&
        rejects_mutable_or_escaping_owner();
    if (!ok) return 1;
    std::puts("OWNERSHIP_SOURCE_LOWERING_V1|positive=1|negative=5|result=PASS");
    return 0;
}
