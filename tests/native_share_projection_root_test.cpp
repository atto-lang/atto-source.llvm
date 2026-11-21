#include "oreslang/types/NativeOwnershipPlace.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {
using K = Expression::Kind;
using oreslang::native_ownership::tracked_share_projection_root;

Expression leaf(K kind, std::string name = {}) {
    Expression result{};
    result.kind = kind;
    result.name = std::move(name);
    return result;
}

Expression fieldOn(Expression root, std::string field = "leaf") {
    Expression result = leaf(K::Field, std::move(field));
    result.left = std::make_unique<Expression>(std::move(root));
    return result;
}

Expression chain(unsigned fields, std::string root = "owner") {
    Expression result = leaf(K::Variable, std::move(root));
    for (unsigned n = 0; n < fields; ++n)
        result = fieldOn(std::move(result), "field");
    return result;
}

bool check(const char* label, const Expression& value, bool shouldHaveOwner,
           const char* expectedName = "owner") {
    const Expression* owner = tracked_share_projection_root(value);
    const bool found = owner != nullptr;
    if (found != shouldHaveOwner ||
            (found && owner->name != expectedName)) {
        std::cerr << "FAIL root projection: " << label << '\n';
        return false;
    }
    return true;
}
}

int main() {
    int count = 0;
    auto verify = [&](const char* label, const Expression& value, bool valid) {
        ++count;
        return check(label, value, valid);
    };
    if (!verify("direct owner", chain(0), true)) return 1;
    if (!verify("unnamed owner", chain(0, ""), false)) return 2;
    if (!verify("one field", chain(1), true)) return 3;
    if (!verify("two fields", chain(2), true)) return 4;
    if (!verify("three fields", chain(3), true)) return 5;
    if (!verify("four fields", chain(4), true)) return 6;
    if (!verify("seven fields", chain(7), true)) return 7;
    if (!verify("eight fields", chain(8), true)) return 8;
    if (!verify("nine fields exceeds bound", chain(9), false)) return 9;
    if (!verify("ten fields exceeds bound", chain(10), false)) return 10;
    if (!verify("missing field child", leaf(K::Field, "field"), false)) return 11;
    if (!verify("unnamed direct field", fieldOn(chain(0), ""), false)) return 12;
    if (!verify("unnamed receiver", fieldOn(chain(0, "")), false)) return 13;
    if (!verify("computed add root", fieldOn(leaf(K::Add)), false)) return 14;
    if (!verify("call root", fieldOn(leaf(K::Call)), false)) return 15;
    if (!verify("borrow root", fieldOn(leaf(K::Borrow)), false)) return 16;
    if (!verify("array root", fieldOn(leaf(K::ArrayCall)), false)) return 17;
    auto invalidIntermediate = chain(3);
    invalidIntermediate.left->name.clear();
    if (!verify("unnamed intermediate field", invalidIntermediate, false)) return 18;
    if (!verify("nested call root", fieldOn(fieldOn(leaf(K::Call))), false)) return 19;
    if (!verify("nested mutable borrow root", fieldOn(fieldOn(leaf(K::BorrowMut))), false))
        return 20;
    if (count != 20) {
        std::cerr << "unexpected native projection case count: " << count << '\n';
        return 21;
    }
    std::cout << "PASS: " << count
              << " bounded native Share root-provenance AST cases (HOST_CPP17_ONLY)\n";
    return 0;
}
