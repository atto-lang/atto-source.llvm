#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::dop {

/**
 * Pure DOP/ECS compiler scheduling metadata, not an access capability.
 *
 * Allocation identities MUST be canonicalized by trusted provenance analysis.
 * nullptr represents unknown provenance, which conflicts with every writer.
 * Intervals are half-open byte offsets [begin, end), not entity indices.
 * This planner never grants a loan, pointer, actor turn or worker execution.
 */
enum class AccessMode { Read, Write };

struct SystemAccessSpan {
    const void* allocation = nullptr;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    AccessMode mode = AccessMode::Read;

    SystemAccessSpan(const void* id, std::uint64_t first,
                     std::uint64_t last, AccessMode access)
        : allocation(id), begin(first), end(last), mode(access) {
        if (first >= last) throw std::invalid_argument("empty ECS byte span");
    }
};

struct SystemFootprint {
    std::string name;
    std::vector<SystemAccessSpan> spans;

    SystemFootprint(std::string system_name, std::vector<SystemAccessSpan> accesses)
        : name(std::move(system_name)), spans(std::move(accesses)) {
        if (name.empty()) throw std::invalid_argument("ECS system name is empty");
    }
};

inline bool access_conflicts(const SystemFootprint& a, const SystemFootprint& b) noexcept {
    for (const auto& left : a.spans) {
        for (const auto& right : b.spans) {
            if (left.mode == AccessMode::Read && right.mode == AccessMode::Read) continue;
            if (left.allocation == nullptr || right.allocation == nullptr) return true;
            if (left.allocation != right.allocation) continue;
            if (left.begin < right.end && right.begin < left.end) return true;
        }
    }
    return false;
}

/**
 * Stable, order-preserving batches. Any conflicting system starts a new batch
 * instead of being backfilled into an earlier one (which could overtake a
 * dependent system). Callers must independently mint, scope and retire
 * disjoint region loans before any workers execute a computed batch.
 */
inline std::vector<std::vector<SystemFootprint>>
plan_access_batches(const std::vector<SystemFootprint>& systems) {
    std::vector<std::vector<SystemFootprint>> batches;
    std::vector<SystemFootprint> current;
    for (const auto& system : systems) {
        bool conflict = false;
        for (const auto& existing : current) {
            if (access_conflicts(existing, system)) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            batches.push_back(std::move(current));
            current.clear();
        }
        current.push_back(system);
    }
    if (!current.empty()) batches.push_back(std::move(current));
    return batches;
}

} // namespace oreslang::dop
