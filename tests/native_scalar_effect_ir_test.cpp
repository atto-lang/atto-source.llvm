#include "oreslang/types/ScalarEffectIr.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

static std::unique_ptr<Expression> expression(Expression::Kind kind,
        std::unique_ptr<Expression> left = {},
        std::unique_ptr<Expression> right = {}) {
    auto out = std::make_unique<Expression>();
    out->kind = kind;
    out->left = std::move(left);
    out->right = std::move(right);
    return out;
}
static Program scalar_program() {
    Program p;
    Function f;
    f.name = "answer";
    f.result = expression(Expression::Kind::Add,
        expression(Expression::Kind::Integer),
        expression(Expression::Kind::Integer));
    p.functions.push_back(std::move(f));
    return p;
}
static bool rejected(const Program& p) {
    try { (void)emit_native_scalar_effect_ir_v1(p); }
    catch (const std::runtime_error&) { return true; }
    return false;
}
int main() {
    const std::string golden =
        "{\"schema_version\":1,\"functions\":[{\"name\":\"answer\",\"operations\":["
        "{\"kind\":\"compute\",\"units\":1},"
        "{\"kind\":\"compute\",\"units\":1},"
        "{\"kind\":\"compute\",\"units\":1}]}]}";
    auto p = scalar_program();
    assert(emit_native_scalar_effect_ir_v1(p) == golden);
    p.functions[0].result = expression(Expression::Kind::Call);
    assert(rejected(p));
    p = scalar_program();
    p.functions[0].parameters.push_back(Parameter{});
    assert(rejected(p));
    p = scalar_program();
    p.functions[0].result = expression(Expression::Kind::Add,
        expression(Expression::Kind::Integer), {});
    assert(rejected(p));
    p = scalar_program();
    p.functions[0].name = "unsafe\"name";
    assert(rejected(p));
    p = scalar_program();
    p.functions[0].name = "main";
    assert(rejected(p));
    p = scalar_program();
    p.functions.emplace_back();
    p.functions.back().name = "answer";
    p.functions.back().result = expression(Expression::Kind::Integer);
    assert(rejected(p));
    p = scalar_program();
    p.actors.emplace_back();
    assert(rejected(p));
    p = scalar_program();
    p.functions[0].has_declared_effects = true;
    assert(rejected(p));
    p = scalar_program();
    for (unsigned i = 0; i < 70; ++i)
        p.functions[0].result = expression(Expression::Kind::Multiply,
            std::move(p.functions[0].result),
            expression(Expression::Kind::Integer));
    assert(rejected(p));  // excessive tree depth
    return 0;
}
