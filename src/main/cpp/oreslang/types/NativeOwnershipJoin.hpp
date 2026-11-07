#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Compiler-only native ownership metadata. No source-level pointer syntax,
// runtime borrowing or allocation authority is introduced by this header.
// A CFG join is safe only if BOTH the affine state and every static
// place/capability classification agree at all incoming predecessors.
namespace oreslang::native_ownership {

using Mode = Parameter::Mode;

// A BindingId identifies one compiler-local declaration and survives CFG
// state cloning. An OriginId is more demanding: zero means no independent
// allocation identity has been proved. It is NEVER inferred from an AST
// identifier or the fact that two lexical slots have different names.
struct BindingId {
    std::uint64_t value = 0;
};
struct OriginId {
    std::uint64_t value = 0;
};

struct LocalOwnership {
    BindingId binding_id{};
    OriginId allocation_origin{};
    Mode mode = Mode::Value;
    bool moved = false;
    bool heap_owned = false; // Experimental region slots cannot transfer ownership yet.
    bool int_array = false; // Non-copy lexical array; no borrows or escapes.
    bool record_view = false; // readonly materialized value, not an incoming loan
    bool mutable_owner = false; // lexical let mut or exclusive borrowed parameter
    bool assignable = false; // lexical slot replacement, not referent mutation
    std::size_t shared_loans = 0;
    bool exclusive_loan = false;
    std::string class_name;
    std::unordered_set<std::string> mutable_paths;
    std::unordered_map<std::string, std::string> subobjects;
    std::unordered_set<std::string> class_fields;
    std::unordered_set<std::string> class_array_fields;
    std::unordered_set<std::string> final_fields;
    std::unordered_set<std::string> public_fields;
    // Primitive i64 class fields can be moved and later restored by a write.
    std::unordered_set<std::string> moved_fields;
};
using Locals = std::unordered_map<std::string, LocalOwnership>;

inline bool same_affine_state(const Locals& left, const Locals& right) {
    if (left.size() != right.size()) return false;
    for (const auto& item : left) {
        const auto found = right.find(item.first);
        if (found == right.end()) return false;
        const LocalOwnership& a = item.second;
        const LocalOwnership& b = found->second;
        if (
            a.mode != b.mode ||
            a.binding_id.value != b.binding_id.value ||
            a.allocation_origin.value != b.allocation_origin.value ||
            a.moved != b.moved ||
            a.heap_owned != b.heap_owned ||
            a.int_array != b.int_array ||
            a.record_view != b.record_view ||
            a.mutable_owner != b.mutable_owner ||
            a.assignable != b.assignable ||
            a.shared_loans != b.shared_loans ||
            a.exclusive_loan != b.exclusive_loan ||
            a.class_name != b.class_name ||
            a.mutable_paths != b.mutable_paths ||
            a.subobjects != b.subobjects ||
            a.class_fields != b.class_fields ||
            a.class_array_fields != b.class_array_fields ||
            a.final_fields != b.final_fields ||
            a.public_fields != b.public_fields ||
            a.moved_fields != b.moved_fields)
            return false;
    }
    return true;
}

} // namespace oreslang::native_ownership
