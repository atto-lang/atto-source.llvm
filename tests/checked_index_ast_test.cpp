#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(1);
}
}

int main() {
    using oreslang::Expression;

    static_assert(Expression::Kind::Index != Expression::Kind::Field,
        "bracket projection authority must remain distinct from named fields");
    static_assert(Expression::Kind::Index != Expression::Kind::ArrayCall,
        "bracket projection authority must not collapse into Array<int> calls");

    auto receiver = std::make_unique<Expression>();
    receiver->kind = Expression::Kind::Variable;
    receiver->name = "value";
    auto selector = std::make_unique<Expression>();
    selector->kind = Expression::Kind::Variable;
    selector->name = "key";

    Expression index{};
    index.kind = Expression::Kind::Index;
    index.left = std::move(receiver);
    index.right = std::move(selector);

    if (!index.left || index.left->kind != Expression::Kind::Variable
            || index.left->name != "value")
        fail("Index receiver identity was not preserved");
    if (!index.right || index.right->kind != Expression::Kind::Variable
            || index.right->name != "key")
        fail("Index selector identity was not preserved");
    if (!index.name.empty())
        fail("Index AST must not reuse Field member-name authority");
    return 0;
}
