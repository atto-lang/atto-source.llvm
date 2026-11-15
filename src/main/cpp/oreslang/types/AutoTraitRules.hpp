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
    // Only compiler-verified, complete, instantiated nominal layouts may set
    // this. An opaque/FFI/user-supplied proof fails closed.
    bool complete_nominal_layout = false;
    std::set<AutoTrait> negative;
};

inline const AutoType& exactly_one(const AutoType& type) {
    if (type.parameters.size() != 1)
        throw std::invalid_argument("ownership auto-trait wrapper requires one type parameter");
    return type.parameters.front();
}

inline Capabilities apply_negative(Capabilities caps, const std::set<AutoTrait>& negative) {
    if (negative.count(AutoTrait::Send)) caps.send = false;
    if (negative.count(AutoTrait::Sync)) caps.sync = false;
    if (negative.count(AutoTrait::Share)) caps.share = false;
    caps.share = caps.share && caps.send && caps.sync;
    return caps;
}

namespace detail {
// Untrusted nested type graphs may not consume unbounded stack or CPU.
// Budget spans the *whole* proof rather than restarting at each nested field.
inline constexpr std::size_t kTypeDepth = 128;
inline constexpr std::size_t kTypeNodes = 4096;

inline Capabilities infer(const AutoType& type, std::size_t depth,
                          std::size_t& remaining) {
    if (depth >= kTypeDepth || remaining == 0) return {};
    --remaining;

    Capabilities caps;
    switch (type.kind) {
    case AutoTypeKind::Scalar:
        // A fabricated "scalar" with hidden Rc/borrowed metadata must never
        // bypass the transitive proof, nor may it carry dyn trait assertions.
        if (!type.parameters.empty() || !type.dyn_bounds.empty() ||
                type.complete_nominal_layout) return {};
        caps = {true, true, true};
        break;

    case AutoTypeKind::Nominal: {
        if (!type.complete_nominal_layout || !type.dyn_bounds.empty()) return {};
        bool send = true, sync = true, share = true;
        for (const auto& argument : type.parameters) {
            const auto child = infer(argument, depth + 1, remaining);
            send = send && child.send;
            sync = sync && child.sync;
            share = share && child.share;
        }
        caps = {send, sync, type.shareable_domain && share};
        break;
    }

    case AutoTypeKind::Box: {
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        const auto child = infer(type.parameters.front(), depth + 1, remaining);
        caps = {child.send, child.sync, false};
        break;
    }

    case AutoTypeKind::Arc:
    case AutoTypeKind::Weak: {
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        const auto child = infer(type.parameters.front(), depth + 1, remaining);
        const bool concurrent = type.shareable_domain && child.send && child.sync;
        caps = {concurrent, concurrent, concurrent && child.share};
        break;
    }

    case AutoTypeKind::Rc:
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        caps = {false, false, false};
        break;

    case AutoTypeKind::RefCell: {
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        const auto child = infer(type.parameters.front(), depth + 1, remaining);
        caps = {child.send, false, false};
        break;
    }

    case AutoTypeKind::Mutex: {
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        const auto child = infer(type.parameters.front(), depth + 1, remaining);
        caps = {child.send, child.send, false};
        break;
    }

    case AutoTypeKind::RwLock: {
        if (type.parameters.size() != 1 || !type.dyn_bounds.empty()) return {};
        const auto child = infer(type.parameters.front(), depth + 1, remaining);
        caps = {child.send, child.send && child.sync, false};
        break;
    }

    case AutoTypeKind::Dyn:
        if (!type.parameters.empty() || type.complete_nominal_layout) return {};
        caps = {
            type.dyn_bounds.count(AutoTrait::Send) != 0,
            type.dyn_bounds.count(AutoTrait::Sync) != 0,
            type.shareable_domain && type.dyn_bounds.count(AutoTrait::Share) != 0
        };
        break;
    }
    return apply_negative(caps, type.negative);
}
} // namespace detail

/**
 * Backend-independent *abstract* Send/Sync/Share composition.
 * This alone is NOT permission to emit guest Atomic/Box/Arc instructions:
 * the compiler must independently prove intrinsic identity and target
 * runtime/allocator ABI readiness. See SendSyncContract.hpp.
 */
inline Capabilities infer_auto_traits(const AutoType& type) {
    std::size_t remaining = detail::kTypeNodes;
    return detail::infer(type, 0, remaining);
}

// Aggregate helper for compiler callers. Use one global visit budget across
// every field; no independent per-child budget reset.
inline bool all_have(const std::vector<AutoType>& values, AutoTrait trait) {
    std::size_t remaining = detail::kTypeNodes;
    for (const auto& value : values) {
        if (!detail::infer(value, 0, remaining).has(trait)) return false;
    }
    return true;
}

} // namespace oreslang::types
