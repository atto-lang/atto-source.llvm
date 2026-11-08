#pragma once

#include <cstddef>
#include <set>
#include <stdexcept>
#include <vector>

namespace oreslang::types {

enum class AutoTrait { Send, Sync, Share };
enum class AutoTypeKind {
    Scalar, Nominal, Box, Arc, Weak, Rc, RefCell, Mutex, RwLock, Dyn
};

struct Capabilities {
    bool send = false;
    bool sync = false;
    bool share = false;

    bool has(AutoTrait trait) const noexcept {
        switch (trait) {
        case AutoTrait::Send: return send;
        case AutoTrait::Sync: return sync;
        case AutoTrait::Share: return share && send && sync;
        }
        return false;
    }
};

struct AutoType {
    AutoTypeKind kind = AutoTypeKind::Scalar;
    std::vector<AutoType> parameters;
    std::set<AutoTrait> dyn_bounds;
    bool shareable_domain = false;
    // Nominal auto traits require a complete instantiated layout proof.
    // Opaque/imported/unresolved nominal provenance fails closed.
    bool complete_nominal_layout = false;
    std::set<AutoTrait> negative;
};

// A single proof budget must flow through every child and trait check.
// Otherwise an attacker-controlled recursive/wide type can reset the limit
// at each nominal field, causing unbounded compilation work or stack depth.
struct AutoTraitProofBudget {
    std::size_t remaining = 4096;
};
inline constexpr std::size_t kAutoTraitMaxDepth = 128;

inline Capabilities infer_auto_traits_bounded(
        const AutoType& type, std::size_t depth, AutoTraitProofBudget& budget);
inline Capabilities infer_auto_traits(const AutoType& type);

inline const AutoType& exactly_one(const AutoType& type) {
    if (type.parameters.size() != 1)
        throw std::invalid_argument("ownership auto-trait wrapper requires one type parameter");
    return type.parameters.front();
}

inline bool all_have_bounded(const std::vector<AutoType>& values,
        AutoTrait trait, std::size_t depth, AutoTraitProofBudget& budget) {
    for (const auto& value : values)
        if (!infer_auto_traits_bounded(value, depth, budget).has(trait)) return false;
    return true;
}

// Preserve the existing direct helper API for host callers; internal nominal
// recursion must use all_have_bounded to retain the shared budget.
inline bool all_have(const std::vector<AutoType>& values, AutoTrait trait) {
    AutoTraitProofBudget budget;
    return all_have_bounded(values, trait, 0, budget);
}

inline Capabilities apply_negative(Capabilities caps, const std::set<AutoTrait>& negative) {
    if (negative.count(AutoTrait::Send)) caps.send = false;
    if (negative.count(AutoTrait::Sync)) caps.sync = false;
    if (negative.count(AutoTrait::Share)) caps.share = false;
    // A cross-actor shared alias must also satisfy Send and Sync.
    // Preserve this invariant after negative-trait erasure, including for
    // dyn and nominal types whose bounds may be contradictory.
    caps.share = caps.share && caps.send && caps.sync;
    return caps;
}

/**
 * Backend-independent Send/Sync/Share composition. Share is intentionally
 * separate from Send: moving a unique owner never grants shared actor-domain
 * aliases to its backing allocation.
 */
inline Capabilities infer_auto_traits_bounded(
        const AutoType& type, std::size_t depth, AutoTraitProofBudget& budget) {
    if (depth >= kAutoTraitMaxDepth || budget.remaining == 0)
        return {false, false, false};
    --budget.remaining;
    Capabilities caps;
    switch (type.kind) {
    case AutoTypeKind::Scalar:
        // Scalar is a compiler-verified leaf. Hidden nested types or dyn
        // bounds must not launder a non-Send payload into a Copy/Send scalar.
        caps = type.parameters.empty() && type.dyn_bounds.empty() &&
                !type.complete_nominal_layout
            ? Capabilities{true, true, true}
            : Capabilities{false, false, false};
        break;
    case AutoTypeKind::Nominal:
        if (!type.complete_nominal_layout) {
            caps = {false, false, false};
            break;
        }
        caps = {
            all_have_bounded(type.parameters, AutoTrait::Send, depth + 1, budget),
            all_have_bounded(type.parameters, AutoTrait::Sync, depth + 1, budget),
            type.shareable_domain && all_have_bounded(type.parameters, AutoTrait::Share, depth + 1, budget)
        };
        break;
    case AutoTypeKind::Box: {
        const auto nested = infer_auto_traits_bounded(exactly_one(type), depth + 1, budget);
        caps = {nested.send, nested.sync, false};
        break;
    }
    case AutoTypeKind::Arc:
    case AutoTypeKind::Weak: {
        const auto nested = infer_auto_traits_bounded(exactly_one(type), depth + 1, budget);
        const bool concurrent =
            type.shareable_domain && nested.send && nested.sync;
        caps = {concurrent, concurrent, concurrent && nested.share};
        break;
    }
    case AutoTypeKind::Rc:
        (void)exactly_one(type);
        caps = {false, false, false};
        break;
    case AutoTypeKind::RefCell: {
        const auto nested = infer_auto_traits_bounded(exactly_one(type), depth + 1, budget);
        caps = {nested.send, false, false};
        break;
    }
    case AutoTypeKind::Mutex: {
        const auto nested = infer_auto_traits_bounded(exactly_one(type), depth + 1, budget);
        caps = {nested.send, nested.send, false};
        break;
    }
    case AutoTypeKind::RwLock: {
        const auto nested = infer_auto_traits_bounded(exactly_one(type), depth + 1, budget);
        caps = {nested.send, nested.send && nested.sync, false};
        break;
    }
    case AutoTypeKind::Dyn:
        // Dyn bounds describe this exact trait object, never hidden
        // additional captured storage. Invalid mixed shapes fail closed.
        caps = type.parameters.empty() && !type.complete_nominal_layout
            ? Capabilities{
                type.dyn_bounds.count(AutoTrait::Send) != 0,
                type.dyn_bounds.count(AutoTrait::Sync) != 0,
                type.shareable_domain && type.dyn_bounds.count(AutoTrait::Share) != 0}
            : Capabilities{false, false, false};
        break;
    }
    return apply_negative(caps, type.negative);
}

inline Capabilities infer_auto_traits(const AutoType& type) {
    AutoTraitProofBudget budget;
    return infer_auto_traits_bounded(type, 0, budget);
}

} // namespace oreslang::types
