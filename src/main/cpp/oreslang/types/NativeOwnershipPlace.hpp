#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/NativeOwnershipJoin.hpp"
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Native stage-one ownership transfer only admits addressable, compiler-
// tracked source places. Computed temporaries, nested/indirect projections
// and call results cannot masquerade as an ownership transfer: their source
// owner is not invalidated by the current lowering.
namespace oreslang::native_ownership {

enum class TakeSourceKind {
    Untracked,
    NamedOwner,
    NamedField
};

inline TakeSourceKind take_source_kind(const Expression& source) noexcept {
    if (source.kind == Expression::Kind::Variable && !source.name.empty())
        return TakeSourceKind::NamedOwner;
    if (source.kind == Expression::Kind::Field && !source.name.empty() &&
            source.left && source.left->kind == Expression::Kind::Variable &&
            !source.left->name.empty())
        return TakeSourceKind::NamedField;
    return TakeSourceKind::Untracked;
}

// A projected Share value must remain tied to a statically named root.
// Unlike taking a field, read-only scalar projections may be nested, but
// they cannot launder the authority of an incoming borrowed root.
// Restrict to a bounded chain of named fields: no call, index, computed
// expression or synthetic/cyclic AST can manufacture an owner identity.
inline const Expression* tracked_share_projection_root(
        const Expression& value) noexcept {
    const Expression* node = &value;
    for (std::size_t depth = 0; depth <= 8; ++depth) {
        if (node->kind == Expression::Kind::Variable)
            return node->name.empty() ? nullptr : node;
        if (node->kind != Expression::Kind::Field ||
                node->name.empty() || !node->left)
            return nullptr;
        node = node->left.get();
    }
    return nullptr;
}

// Compiler-resolved, stage-one place identity. This is an HIR/CFG bridge
// rather than a complete typed MIR. The root MUST resolve in the current
// compiler Locals map; a caller-supplied identifier string alone is not
// ownership evidence, and this record never proves heap alias separation.
enum class PlaceOrigin {
    OwnedSlot,
    IncomingSharedLoan,
    IncomingExclusiveLoan
};

struct ResolvedPlace {
    std::string lexical_slot;
    std::vector<std::string> fields; // source order, outermost to innermost
    PlaceOrigin origin = PlaceOrigin::OwnedSlot;
    BindingId binding_id{};
    OriginId allocation_origin{};

    // An allocated root can be identified only when the production
    // constructor/move path supplied its provenance. No disjointness
    // conclusions are drawn from differing BindingId values.
    bool has_proven_allocation() const noexcept {
        return allocation_origin.value != 0;
    }

    bool owns_storage() const noexcept { return origin == PlaceOrigin::OwnedSlot; }
    bool has_direct_field() const noexcept { return fields.size() == 1; }
    bool is_named_slot() const noexcept { return fields.empty(); }
};

// A fixed bound prevents recursively forged AST paths or overly deep field
// expressions from escaping the same untracked-projection safety gate.
inline std::optional<ResolvedPlace> resolve_compiler_place(
        const Expression& value, const Locals& locals) {
    const Expression* node = &value;
    std::vector<std::string> fields;
    for (std::size_t depth = 0; depth <= 8; ++depth) {
        if (node->kind == Expression::Kind::Variable) {
            if (node->name.empty() || node->name.find('.') != std::string::npos)
                return std::nullopt;
            const auto found = locals.find(node->name);
            if (found == locals.end() || found->second.binding_id.value == 0)
                return std::nullopt;
            std::reverse(fields.begin(), fields.end());
            PlaceOrigin provenance = PlaceOrigin::OwnedSlot;
            if (found->second.mode == Parameter::Mode::Borrow)
                provenance = PlaceOrigin::IncomingSharedLoan;
            else if (found->second.mode == Parameter::Mode::BorrowMut)
                provenance = PlaceOrigin::IncomingExclusiveLoan;
            return ResolvedPlace{found->first, std::move(fields), provenance,
                found->second.binding_id, found->second.allocation_origin};
        }
        if (node->kind != Expression::Kind::Field || node->name.empty() ||
                node->name.find('.') != std::string::npos || !node->left)
            return std::nullopt;
        fields.push_back(node->name);
        node = node->left.get();
    }
    return std::nullopt;
}

// The flat CFG is keyed by compiler-generated canonical source slots and
// validated field paths. Centralize serialization so the checker and graph
// builder cannot silently disagree on which storage is invalidated.
inline std::string compiler_place_key(const ResolvedPlace& place) {
    std::string key = place.lexical_slot;
    for (const auto& field : place.fields) {
        key.push_back('.');
        key += field;
    }
    return key;
}

// Sharing a named local or a named class field must originate in an owned
// value slot. A shared/mutable borrowed parameter is a loan; Copy of its
// scalar payload is separate from sharing the loan itself. This predicate is
// called by the real native ownership checker for both source categories.
inline bool share_place_has_owner_authority(Parameter::Mode mode) noexcept {
    return mode == Parameter::Mode::Value;
}

} // namespace oreslang::native_ownership
