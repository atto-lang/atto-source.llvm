#include "oreslang/types/NativeOwnershipPlace.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using oreslang::native_ownership::Locals;
using oreslang::native_ownership::LocalOwnership;
using oreslang::native_ownership::PlaceOrigin;
using oreslang::native_ownership::resolve_compiler_place;
using oreslang::native_ownership::same_affine_state;
using oreslang::native_ownership::validate_identity_inventory;
using Mode = Parameter::Mode;

LocalOwnership slot(std::uint64_t binding, std::uint64_t origin = 0,
                    Mode mode = Mode::Value, bool moved = false) {
    LocalOwnership value;
    value.binding_id.value = binding;
    value.allocation_origin.value = origin;
    value.mode = mode;
    value.moved = moved;
    return value;
}
Locals one(std::uint64_t binding = 1, std::uint64_t origin = 0,
           Mode mode = Mode::Value, bool moved = false) {
    return {{"owner", slot(binding, origin, mode, moved)}};
}
bool accepted(const Locals& locals) {
    try { validate_identity_inventory(locals); return true; }
    catch (const std::runtime_error&) { return false; }
}
bool rejected(const Locals& locals) { return !accepted(locals); }
Expression name(const char* id) {
    Expression expr{};
    expr.kind = Expression::Kind::Variable;
    expr.name = id;
    return expr;
}
Expression field(const char* root, const char* part) {
    Expression expr{};
    expr.kind = Expression::Kind::Field;
    expr.name = part;
    expr.left = std::make_unique<Expression>(name(root));
    return expr;
}
bool throws_lookup(const Locals& locals) {
    try { (void)resolve_compiler_place(name("owner"), locals); return false; }
    catch (const std::runtime_error&) { return true; }
}
} // namespace

int main() {
    unsigned count = 0;
    auto verify = [&](const char* label, bool condition) {
        ++count;
        if (!condition)
            std::cerr << "FAIL identity/CFG " << count << ": " << label << '\n';
        return condition;
    };
    auto base = one(1, 10);
    Locals second = base; second.emplace("other", slot(2, 11));
    Locals same_origin = base; same_origin.emplace("other", slot(2, 10));
    Locals moved_source = one(1, 10, Mode::Value, true);
    moved_source.emplace("destination", slot(2, 10));

    // BindingId: declaration identity, not names or heap identity (5).
    if (!verify("minted owner", accepted(base))) return 1;
    if (!verify("two distinct minted slots", accepted(second))) return 2;
    if (!verify("zero binding denied", rejected(one(0, 10)))) return 3;
    Locals duplicate_id = base; duplicate_id.emplace("alias", slot(1, 0, Mode::Borrow));
    if (!verify("duplicate lexical ID denied", rejected(duplicate_id))) return 4;
    Locals empty_name = base; empty_name.emplace("", slot(3));
    if (!verify("empty root name denied", rejected(empty_name))) return 5;

    // OriginId: only one live owned slot per known origin (5).
    if (!verify("unknown allocation stays admissible", accepted(one(1)))) return 6;
    if (!verify("two independently known origins", accepted(second))) return 7;
    if (!verify("duplicate live owned origin denied", rejected(same_origin))) return 8;
    if (!verify("transfer preserves origin with moved predecessor", accepted(moved_source))) return 9;
    Locals loan_alias = base; loan_alias.emplace("loan", slot(2, 10, Mode::Borrow));
    if (!verify("borrowed view may carry owner origin", accepted(loan_alias))) return 10;

    // CFG joins compare exact identities AND affine state (5).
    if (!verify("cloned state joins", same_affine_state(base, base))) return 11;
    auto other_binding = base; other_binding.at("owner").binding_id.value = 3;
    if (!verify("different binding cannot join", !same_affine_state(base, other_binding))) return 12;
    auto other_origin = base; other_origin.at("owner").allocation_origin.value = 12;
    if (!verify("different allocation source cannot join", !same_affine_state(base, other_origin))) return 13;
    auto moved = base; moved.at("owner").moved = true;
    if (!verify("move on one branch cannot join", !same_affine_state(base, moved))) return 14;
    auto shared = base; shared.at("owner").shared_loans = 1;
    if (!verify("loan count differs at join", !same_affine_state(base, shared))) return 15;

    // Resolve the real source storage, not merely the source spelling (5).
    const auto owned_root = resolve_compiler_place(name("owner"), base);
    if (!verify("owned binding has ID", owned_root && owned_root->binding_id.value == 1)) return 16;
    const auto projected = resolve_compiler_place(field("owner", "count"), base);
    if (!verify("projection retains allocation origin", projected &&
            projected->allocation_origin.value == 10 && projected->fields.size() == 1)) return 17;
    if (!verify("unminted lookup denied", throws_lookup(one(0)))) return 18;
    if (!verify("duplicate binding lookup denied", throws_lookup(duplicate_id))) return 19;
    if (!verify("conflicting allocation lookup denied", throws_lookup(same_origin))) return 20;

    // Borrow modes never manufacture new ownership authority (5).
    auto immutable = one(1, 0, Mode::Borrow);
    auto exclusive = one(1, 0, Mode::BorrowMut);
    const auto shared_loan = resolve_compiler_place(name("owner"), immutable);
    if (!verify("shared loan origin", shared_loan &&
            shared_loan->origin == PlaceOrigin::IncomingSharedLoan &&
            !shared_loan->owns_storage())) return 21;
    const auto mut_loan = resolve_compiler_place(name("owner"), exclusive);
    if (!verify("exclusive loan origin", mut_loan &&
            mut_loan->origin == PlaceOrigin::IncomingExclusiveLoan &&
            !mut_loan->owns_storage())) return 22;
    auto borrowed_known_origin = one(1, 10, Mode::Borrow);
    const auto borrowed_place = resolve_compiler_place(name("owner"), borrowed_known_origin);
    if (!verify("borrow with known origin still lacks owner capability",
            borrowed_place && !borrowed_place->owns_storage())) return 23;
    auto exclusive_known_origin = one(1, 10, Mode::BorrowMut);
    const auto exclusive_place = resolve_compiler_place(name("owner"), exclusive_known_origin);
    if (!verify("exclusive loan with origin still lacks independent owner",
            exclusive_place && !exclusive_place->owns_storage())) return 24;
    const auto unknown_alloc = resolve_compiler_place(name("owner"), one());
    if (!verify("owned binding cannot invent heap origin",
            unknown_alloc && unknown_alloc->owns_storage() &&
            !unknown_alloc->has_proven_allocation())) return 25;

    // Adversarial transfer and ancestor/descendant identity invariants (5).
    const auto old_place = resolve_compiler_place(name("owner"), moved_source);
    const auto new_place = resolve_compiler_place(name("destination"), moved_source);
    if (!verify("moved predecessor still tracks same origin",
            old_place && new_place &&
            old_place->allocation_origin.value == new_place->allocation_origin.value &&
            moved_source.at("owner").moved)) return 26;
    Locals clone = second;
    if (!verify("two-binding CFG clone identical", same_affine_state(second, clone))) return 27;
    if (!verify("same origin two live lexical names cannot imply disjointness",
            rejected(same_origin))) return 28;
    if (!verify("owner plus borrowed alias not treated as second owner",
            accepted(loan_alias))) return 29;
    auto moved_both = moved_source; moved_both.at("destination").moved = true;
    if (!verify("all historical slots moved: no live allocation conflict",
            accepted(moved_both))) return 30;

    if (count != 30) return 31;
    std::cout << "PASS 30/30 compiler-local identity and CFG invariants (HOST_CPP17_ONLY)\n";
    return 0;
}
