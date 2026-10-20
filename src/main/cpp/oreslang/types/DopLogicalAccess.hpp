#pragma once

#include "oreslang/runtime/DopLayout.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types::dop_access {

enum class Access { Read, Write };

struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    Range(std::uint64_t first, std::uint64_t last) : begin(first), end(last) {
        if (first >= last) throw std::invalid_argument("invalid DOP logical range");
    }

    bool overlaps(const Range& other) const noexcept {
        return begin < other.end && other.begin < end;
    }
};

/**
 * Logical ownership place independent of AoS/SoA/AoSoA byte geometry.
 * Missing hierarchy elements denote the whole parent place.
 */
struct Place {
    oreslang::dop::Layout layout = oreslang::dop::Layout::AoS;
    std::string store;
    std::optional<std::string> archetype;
    std::optional<std::string> component;
    std::optional<std::string> field;
    std::optional<Range> rows;
    std::uint64_t epoch = 0;

    Place(oreslang::dop::Layout physical_layout,
          std::string store_id,
          std::optional<std::string> archetype_id,
          std::optional<std::string> component_id,
          std::optional<std::string> field_id,
          std::optional<Range> row_range,
          std::uint64_t storage_epoch)
        : layout(physical_layout),
          store(std::move(store_id)),
          archetype(std::move(archetype_id)),
          component(std::move(component_id)),
          field(std::move(field_id)),
          rows(std::move(row_range)),
          epoch(storage_epoch) {
        if (store.empty()) throw std::invalid_argument("DOP place requires store identity");
        if (!component && field)
            throw std::invalid_argument("DOP field requires component identity");
    }
};

struct Loan {
    Place place;
    Access access = Access::Read;
};

inline bool same_or_whole(const std::optional<std::string>& left,
                          const std::optional<std::string>& right) noexcept {
    return !left || !right || *left == *right;
}

inline void require_current_epoch(const Place& place, std::uint64_t current_epoch) {
    if (place.epoch != current_epoch)
        throw std::invalid_argument("stale DOP logical borrow epoch");
}

inline bool overlaps(const Place& left, const Place& right) noexcept {
    if (left.store != right.store) return false;
    // Epoch mismatch must never be interpreted as safe disjointness. One side
    // is stale and admission must reject it with require_current_epoch().
    if (left.epoch != right.epoch) return true;
    if (!same_or_whole(left.archetype, right.archetype)) return false;
    if (!same_or_whole(left.component, right.component)) return false;
    if (!same_or_whole(left.field, right.field)) return false;
    return !left.rows || !right.rows || left.rows->overlaps(*right.rows);
}

inline bool conflicts(const Loan& left, const Loan& right) noexcept {
    return overlaps(left.place, right.place)
        && (left.access == Access::Write || right.access == Access::Write);
}

inline bool can_run_concurrently(const std::vector<Loan>& left,
                                 const std::vector<Loan>& right) noexcept {
    for (const auto& a : left)
        for (const auto& b : right)
            if (conflicts(a, b)) return false;
    return true;
}

inline void require_internally_valid(const std::vector<Loan>& loans) {
    for (std::size_t i = 0; i < loans.size(); ++i)
        for (std::size_t j = i + 1; j < loans.size(); ++j)
            if (conflicts(loans[i], loans[j]))
                throw std::invalid_argument("conflicting DOP logical loans");
}

} // namespace oreslang::types::dop_access
