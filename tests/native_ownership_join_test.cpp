#include "oreslang/types/NativeOwnershipJoin.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

using oreslang::native_ownership::LocalOwnership;
using oreslang::native_ownership::Locals;
using oreslang::native_ownership::same_affine_state;

struct Mutation {
    const char* field;
    std::function<void(LocalOwnership&)> apply;
};

int main() {
    Locals root{{"owner", LocalOwnership{}}};
    root.at("owner").binding_id.value = 1;
    if (!same_affine_state(root, root)) return 1;

    // Each metadata dimension is independently relevant to borrowing,
    // codegen, and/or source-place invalidation. Dropping ANY comparison
    // from the production join must break this test.
    const std::vector<Mutation> mutations = {
        {"mode", [](auto& x) { x.mode = oreslang::Parameter::Mode::Borrow; }},
        {"binding_id", [](auto& x) { x.binding_id.value = 2; }},
        {"allocation_origin", [](auto& x) { x.allocation_origin.value = 7; }},
        {"moved", [](auto& x) { x.moved = true; }},
        {"heap_owned", [](auto& x) { x.heap_owned = true; }},
        {"int_array", [](auto& x) { x.int_array = true; }},
        {"record_view", [](auto& x) { x.record_view = true; }},
        {"mutable_owner", [](auto& x) { x.mutable_owner = true; }},
        {"assignable", [](auto& x) { x.assignable = true; }},
        {"shared_loans", [](auto& x) { x.shared_loans = 1; }},
        {"exclusive_loan", [](auto& x) { x.exclusive_loan = true; }},
        {"class_name", [](auto& x) { x.class_name = "BoundedOwner"; }},
        {"mutable_paths", [](auto& x) { x.mutable_paths.insert("slot"); }},
        {"subobjects", [](auto& x) { x.subobjects.emplace("slot", "Child"); }},
        {"class_fields", [](auto& x) { x.class_fields.insert("slot"); }},
        {"class_array_fields", [](auto& x) { x.class_array_fields.insert("slot"); }},
        {"final_fields", [](auto& x) { x.final_fields.insert("slot"); }},
        {"public_fields", [](auto& x) { x.public_fields.insert("slot"); }},
        {"moved_fields", [](auto& x) { x.moved_fields.insert("slot"); }}
    };

    for (const auto& mutation : mutations) {
        Locals changed = root;
        mutation.apply(changed.at("owner"));
        if (same_affine_state(root, changed) || same_affine_state(changed, root)) {
            std::cerr << "ownership join wrongly accepted different " << mutation.field << '\n';
            return 2;
        }
        Locals matching = changed;
        if (!same_affine_state(changed, matching)) {
            std::cerr << "ownership join rejected identical " << mutation.field << '\n';
            return 3;
        }
    }

    Locals with_sibling = root;
    LocalOwnership sibling;
    sibling.binding_id.value = 2;
    with_sibling.emplace("sibling", sibling);
    if (same_affine_state(root, with_sibling) ||
        same_affine_state(with_sibling, root)) return 4;
    with_sibling.erase("owner");
    if (same_affine_state(root, with_sibling)) return 5;

    std::cout << "PASS: 19 ownership metadata join mutations, symmetric mismatch, matching states, and root inventory\n";
    return 0;
}
