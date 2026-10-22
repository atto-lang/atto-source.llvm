#pragma once

#include <algorithm>
#include <cctype>
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

class SystemAccessSpan {
public:
    SystemAccessSpan(const void* id, std::uint64_t first,
                     std::uint64_t last, AccessMode access)
        : allocation_(id), begin_(first), end_(last), mode_(access) {
        if (first >= last) throw std::invalid_argument("empty ECS byte span");
        if (access != AccessMode::Read && access != AccessMode::Write)
            throw std::invalid_argument("invalid ECS access mode");
    }

    const void* allocation() const noexcept { return allocation_; }
    std::uint64_t begin() const noexcept { return begin_; }
    std::uint64_t end() const noexcept { return end_; }
    AccessMode mode() const noexcept { return mode_; }

private:
    // Private fields prevent callers from mutating a validated interval into
    // an invalid/overlapping interval or forging an out-of-range mode.
    const void* allocation_;
    std::uint64_t begin_;
    std::uint64_t end_;
    AccessMode mode_;
};

class SystemFootprint {
public:
    SystemFootprint(std::string system_name, std::vector<SystemAccessSpan> accesses)
        : name_(std::move(system_name)), spans_(std::move(accesses)) {
        if (name_.empty() || std::all_of(name_.begin(), name_.end(),
                [](unsigned char c) { return std::isspace(c) != 0; }))
            throw std::invalid_argument("ECS system name is empty");
    }

    const std::string& name() const noexcept { return name_; }
    const std::vector<SystemAccessSpan>& spans() const noexcept { return spans_; }

private:
    std::string name_;
    std::vector<SystemAccessSpan> spans_;
};

inline bool access_conflicts(const SystemFootprint& a, const SystemFootprint& b) noexcept {
    // Until a separate compiler effect proof is wired in, "no declared
    // accesses" means unknown side effects, not proven thread-safe purity.
    if (a.spans().empty() || b.spans().empty()) return true;
    for (const auto& left : a.spans()) {
        for (const auto& right : b.spans()) {
            if (left.mode() == AccessMode::Read && right.mode() == AccessMode::Read) continue;
            if (left.allocation() == nullptr || right.allocation() == nullptr) return true;
            if (left.allocation() != right.allocation()) continue;
            if (left.begin() < right.end() && right.begin() < left.end()) return true;
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
