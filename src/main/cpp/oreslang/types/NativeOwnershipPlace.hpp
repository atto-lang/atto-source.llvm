#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <cstddef>

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

// Sharing a named local or a named class field must originate in an owned
// value slot. A shared/mutable borrowed parameter is a loan; Copy of its
// scalar payload is separate from sharing the loan itself. This predicate is
// called by the real native ownership checker for both source categories.
inline bool share_place_has_owner_authority(Parameter::Mode mode) noexcept {
    return mode == Parameter::Mode::Value;
}

} // namespace oreslang::native_ownership
