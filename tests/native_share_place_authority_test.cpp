#include "oreslang/types/NativeOwnershipPlace.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using oreslang::native_ownership::TakeSourceKind;
using oreslang::native_ownership::take_source_kind;
using oreslang::native_ownership::share_place_has_owner_authority;

namespace {
using K = Expression::Kind;
using M = Parameter::Mode;

Expression named(K kind, std::string name) {
    Expression e{};
    e.kind = kind;
    e.name = std::move(name);
    return e;
}

struct Scenario {
    const char* description;
    K kind;
    const char* name;
    K root_kind;
    const char* root_name;
    M mode;
    TakeSourceKind tracked_place;
    bool owner_authority;
};

// Ten independent scalar/direct-field authority classifications. This
// classifies a place and its param origin, not arbitrary computed value
// sharing, Send, or guest runtime safety.
const std::array<Scenario,10> scenarios = {{
    {"owned named scalar", K::Variable, "value", K::Integer, "", M::Value,
        TakeSourceKind::NamedOwner, true},
    {"borrowed named scalar", K::Variable, "value", K::Integer, "", M::Borrow,
        TakeSourceKind::NamedOwner, false},
    {"exclusive borrowed scalar", K::Variable, "value", K::Integer, "", M::BorrowMut,
        TakeSourceKind::NamedOwner, false},
    {"owned direct field", K::Field, "count", K::Variable, "box", M::Value,
        TakeSourceKind::NamedField, true},
    {"borrowed direct field", K::Field, "count", K::Variable, "box", M::Borrow,
        TakeSourceKind::NamedField, false},
    {"exclusive borrowed field", K::Field, "count", K::Variable, "box", M::BorrowMut,
        TakeSourceKind::NamedField, false},
    {"nested non-direct projection", K::Field, "leaf", K::Field, "nested", M::Value,
        TakeSourceKind::Untracked, true},
    {"array index has no tracked take slot", K::ArrayCall, "", K::Variable, "box", M::Value,
        TakeSourceKind::Untracked, true},
    {"computed arithmetic has no tracked take slot", K::Add, "", K::Variable, "box", M::Value,
        TakeSourceKind::Untracked, true},
    {"unnamed field cannot identify move path", K::Field, "", K::Variable, "box", M::Value,
        TakeSourceKind::Untracked, true}
}};
}

int main() {
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const Scenario& row = scenarios[i];
        auto candidate = named(row.kind, row.name);
        if (row.kind != K::Variable)
            candidate.left = std::make_unique<Expression>(named(row.root_kind, row.root_name));
        const auto got = take_source_kind(candidate);
        const bool has_owner = share_place_has_owner_authority(row.mode);
        if (got != row.tracked_place || has_owner != row.owner_authority) {
            std::cerr << "FAIL scenario " << i + 1 << ": " << row.description
                      << " (tracked=" << static_cast<int>(got)
                      << ", owning=" << has_owner << ")\n";
            return static_cast<int>(i + 1);
        }
    }
    std::cout << "PASS: 10 tracked-place and borrowed-owner authority scenarios\n";
    return 0;
}
