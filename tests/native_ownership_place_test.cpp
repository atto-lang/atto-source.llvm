#include "oreslang/types/NativeOwnershipPlace.hpp"

#include <iostream>
#include <memory>
#include <string>

using oreslang::Expression;
using oreslang::native_ownership::TakeSourceKind;
using oreslang::native_ownership::take_source_kind;

static Expression expression(Expression::Kind kind, std::string name = {}) {
    Expression result{};
    result.kind = kind;
    result.name = std::move(name);
    return result;
}

static bool expect(const Expression& expr, TakeSourceKind expected, const char* label) {
    if (take_source_kind(expr) != expected) {
        std::cerr << "unexpected rt take source admission: " << label << '\n';
        return false;
    }
    return true;
}

int main() {
    using K = Expression::Kind;
    if (!expect(expression(K::Variable, "owner"), TakeSourceKind::NamedOwner, "named owner")) return 1;
    if (!expect(expression(K::Variable), TakeSourceKind::Untracked, "unnamed owner")) return 2;

    auto direct = expression(K::Field, "leaf");
    direct.left = std::make_unique<Expression>(expression(K::Variable, "owner"));
    if (!expect(direct, TakeSourceKind::NamedField, "direct named class field")) return 3;

    auto nested = expression(K::Field, "inner");
    nested.left = std::make_unique<Expression>(expression(K::Field, "outer"));
    nested.left->left = std::make_unique<Expression>(expression(K::Variable, "owner"));
    if (!expect(nested, TakeSourceKind::Untracked, "nested field without slot tracking")) return 4;

    auto malformed = expression(K::Field, "leaf");
    if (!expect(malformed, TakeSourceKind::Untracked, "field missing root")) return 5;

    auto unnamed = expression(K::Field, "leaf");
    unnamed.left = std::make_unique<Expression>(expression(K::Variable));
    if (!expect(unnamed, TakeSourceKind::Untracked, "field with unnamed root")) return 6;

    for (const auto kind : {
        K::Integer, K::Boolean, K::Add, K::Subtract, K::Call, K::Conditional,
        K::Copy, K::Share, K::Take, K::Negate, K::LogicalNot, K::Borrow, K::BorrowMut,
        K::ArrayCall, K::RecordConstruct, K::SumConstruct, K::SumAccess
    }) {
        if (!expect(expression(kind), TakeSourceKind::Untracked, "computed or capability expression"))
            return 7;
    }

    std::cout << "PASS: 2 direct source categories and 21 malformed/computed source alternatives\n";
    return 0;
}
