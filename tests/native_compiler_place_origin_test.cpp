#include "oreslang/types/NativeOwnershipPlace.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {
using K = Expression::Kind;
using Mode = Parameter::Mode;
using oreslang::native_ownership::Locals;
using oreslang::native_ownership::LocalOwnership;
using oreslang::native_ownership::PlaceOrigin;
using oreslang::native_ownership::resolve_compiler_place;
using oreslang::native_ownership::compiler_place_key;

Expression make(K kind, std::string name) {
    Expression node{};
    node.kind = kind;
    node.name = std::move(name);
    return node;
}

Expression projected(std::string root, unsigned depth) {
    Expression node = make(K::Variable, std::move(root));
    for (unsigned n = 0; n < depth; ++n) {
        Expression wrapper = make(K::Field, "member");
        wrapper.left = std::make_unique<Expression>(std::move(node));
        node = std::move(wrapper);
    }
    return node;
}

bool expect(const char* description, const Expression& expression,
        const Locals& locals, bool tracked, PlaceOrigin origin,
        unsigned expected_fields, std::size_t& executed) {
    ++executed;
    const auto result = resolve_compiler_place(expression, locals);
    if (result.has_value() != tracked ||
            (result && (result->origin != origin ||
                result->fields.size() != expected_fields ||
                result->lexical_slot != "owner" ||
                result->binding_id.value != 1 ||
                result->allocation_origin.value != 0 ||
                result->has_proven_allocation() ||
                result->owns_storage() != (origin == PlaceOrigin::OwnedSlot)))) {
        std::cerr << "native compiler-resolved place failure: " << description
                  << " case " << executed << '\n';
        return false;
    }
    if (result) {
        std::string expected = "owner";
        for (unsigned index = 0; index < expected_fields; ++index)
            expected += ".member";
        if (compiler_place_key(*result) != expected) {
            std::cerr << "native CFG source-place key differs from checker at "
                      << executed << '\n';
            return false;
        }
    }
    return true;
}
}

int main() {
    Locals owned, shared, exclusive;
    LocalOwnership value;
    // Production IDs are minted from the current function's lexical table.
    // Synthetic test fixtures must declare the binding explicitly as well.
    value.binding_id.value = 1;
    value.mode = Mode::Value;
    owned.emplace("owner", value);
    value.mode = Mode::Borrow;
    shared.emplace("owner", value);
    value.mode = Mode::BorrowMut;
    exclusive.emplace("owner", value);
    // An unminted lexical slot cannot acquire ownership authority just by
    // being inserted in the map under a plausible name.
    Locals unminted = owned;
    unminted.at("owner").binding_id.value = 0;
    if (resolve_compiler_place(projected("owner", 0), unminted)) {
        std::cerr << "unminted AST name acquired compiler-owned provenance\n";
        return 10;
    }
    // Borrowed roots cannot create allocation provenance from a different
    // lexical binding identity; zero remains explicitly unproven.
    if (resolve_compiler_place(projected("owner", 0), shared)
            ->allocation_origin.value != 0)
        return 11;
    std::size_t count = 0;

    // 27 actual AST -> compiler Locals -> typed Place cases:
    // three provenance modes, each at projection depths 0 through 8.
    const Locals* frames[] = {&owned, &shared, &exclusive};
    const PlaceOrigin modes[] = {
        PlaceOrigin::OwnedSlot,
        PlaceOrigin::IncomingSharedLoan,
        PlaceOrigin::IncomingExclusiveLoan
    };
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned depth = 0; depth <= 8; ++depth) {
            const auto expr = projected("owner", depth);
            if (!expect("lexical resolved field chain", expr, *frames[mode],
                    true, modes[mode], depth, count)) return 1;
        }
    }
    // 18 negative source-identity cases. Neither an unknown root nor
    // an empty root can acquire ownership by spelling it in an AST.
    for (unsigned depth = 0; depth <= 8; ++depth) {
        if (!expect("undeclared lexical root", projected("foreign", depth),
                    owned, false, PlaceOrigin::OwnedSlot, 0, count)) return 2;
        if (!expect("unnamed lexical root", projected("", depth),
                    owned, false, PlaceOrigin::OwnedSlot, 0, count)) return 3;
    }
    // Five additional fail-closed projection shapes = exactly 50 cases.
    if (!expect("nine fields above proof bound", projected("owner", 9),
            owned, false, PlaceOrigin::OwnedSlot, 0, count)) return 4;
    if (!expect("computed expression is not source storage",
            make(K::Add, "owner"), owned, false, PlaceOrigin::OwnedSlot, 0, count))
        return 5;
    if (!expect("call cannot manufacture storage origin",
            make(K::Call, "owner"), owned, false, PlaceOrigin::OwnedSlot, 0, count))
        return 6;
    if (!expect("dotted source root cannot impersonate a projection",
            projected("owner.field", 0), owned,
            false, PlaceOrigin::OwnedSlot, 0, count)) return 7;
    Expression malformed = make(K::Field, "field");
    malformed.left = std::make_unique<Expression>(make(K::Variable, "owner"));
    malformed.name.clear();
    if (!expect("empty projection name has no canonical place", malformed,
            owned, false, PlaceOrigin::OwnedSlot, 0, count)) return 8;

    if (count != 50) {
        std::cerr << "expected exactly 50 native compiler place cases; got "
                  << count << '\n';
        return 9;
    }
    std::cout << "PASS: 50 native compiler-resolved lexical ownership origin "
                 "cases (HOST_CPP17_ONLY)\n";
    return 0;
}
