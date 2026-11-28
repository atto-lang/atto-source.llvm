#pragma once

#include "oreslang/runtime/DopLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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

    void validate() const {
        if (begin >= end) throw std::invalid_argument("invalid DOP logical range");
    }

    bool overlaps(const Range& other) const {
        validate();
        other.validate();
        return begin < other.end && other.begin < end;
    }
};

// Match Java String.isBlank for UTF-8 identities. Non-whitespace bytes remain
// identity bytes; this helper does not normalize or reinterpret identifiers.
inline bool blank_identity(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == 0x20 || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f)) {
            ++i;
        } else if (value.compare(i, 3, "\xe1\x9a\x80") == 0
                || value.compare(i, 3, "\xe2\x80\xa8") == 0
                || value.compare(i, 3, "\xe2\x80\xa9") == 0
                || value.compare(i, 3, "\xe2\x81\x9f") == 0
                || value.compare(i, 3, "\xe3\x80\x80") == 0) {
            i += 3;
        } else if (i + 2 < value.size() && c == 0xe2
                && static_cast<unsigned char>(value[i + 1]) == 0x80
                && ((static_cast<unsigned char>(value[i + 2]) >= 0x80
                     && static_cast<unsigned char>(value[i + 2]) <= 0x86)
                    || (static_cast<unsigned char>(value[i + 2]) >= 0x88
                        && static_cast<unsigned char>(value[i + 2]) <= 0x8a))) {
            i += 3;
        } else {
            return false;
        }
    }
    return true;
}

inline constexpr std::uint64_t max_write_granule = 1024;

/**
 * Logical ownership place independent of AoS/SoA/AoSoA byte geometry.
 * Missing hierarchy elements denote the whole parent place.
 *
 * `granule` is how many consecutive rows a writer touches as one unit: the
 * AoSoA lane count for tile-wide stores, the vector width for unmasked SIMD
 * stores over SoA columns, or 1 for scalar access. Logically disjoint ranges
 * that split a granule are not independently writable (a full-tile store to
 * rows [8,16) clobbers rows [12,16) owned by someone else), so write conflicts
 * compare ranges widened to granule boundaries. Mirrors Java
 * DataOrientedBorrowChecker.Place.
 */
struct Place {
    oreslang::dop::Layout layout = oreslang::dop::Layout::AoS;
    std::string store;
    std::optional<std::string> archetype;
    std::optional<std::string> component;
    std::optional<std::string> field;
    std::optional<Range> rows;
    std::uint64_t epoch = 0;
    std::uint64_t granule = 1;

    Place(oreslang::dop::Layout physical_layout,
          std::string store_id,
          std::optional<std::string> archetype_id,
          std::optional<std::string> component_id,
          std::optional<std::string> field_id,
          std::optional<Range> row_range,
          std::uint64_t storage_epoch,
          std::uint64_t write_granule = 1)
        : layout(physical_layout),
          store(std::move(store_id)),
          archetype(std::move(archetype_id)),
          component(std::move(component_id)),
          field(std::move(field_id)),
          rows(std::move(row_range)),
          epoch(storage_epoch),
          granule(write_granule) {
        validate();
    }

    void validate() const {
        switch (layout) {
            case oreslang::dop::Layout::AoS:
            case oreslang::dop::Layout::SoA:
            case oreslang::dop::Layout::AoSoA: break;
            default: throw std::invalid_argument("invalid DOP layout");
        }
        if (rows) rows->validate();
        if (granule == 0 || granule > max_write_granule || (granule & (granule - 1)) != 0)
            throw std::invalid_argument("DOP write granule must be a power of two in 1..1024");
        if (blank_identity(store)) throw std::invalid_argument("DOP place requires store identity");
        auto require_nonblank = [](const std::optional<std::string>& value,
                                   const char* label) {
            if (value && blank_identity(*value))
                throw std::invalid_argument(
                    std::string("DOP ") + label + " identity cannot be blank");
        };
        require_nonblank(archetype, "archetype");
        require_nonblank(component, "component");
        require_nonblank(field, "field");
        if (!component && field)
            throw std::invalid_argument("DOP field requires component identity");
    }
};

struct Loan {
    Place place;
    Access access = Access::Read;

    void validate() const {
        place.validate();
        if (access != Access::Read && access != Access::Write)
            throw std::invalid_argument("invalid DOP access");
    }
};

inline bool same_or_whole(const std::optional<std::string>& left,
                          const std::optional<std::string>& right) noexcept {
    return !left || !right || *left == *right;
}

inline void require_current_epoch(const Place& place, std::uint64_t current_epoch) {
    place.validate();
    if (place.epoch != current_epoch)
        throw std::invalid_argument("stale DOP logical borrow epoch");
}

inline Range widen_to_granule(const Range& range, std::uint64_t granule) {
    if (granule == 1) return range;
    const auto begin = range.begin - range.begin % granule;
    const auto rem = range.end % granule;
    const auto pad = rem == 0 ? 0 : granule - rem;
    const auto end = range.end > std::numeric_limits<std::uint64_t>::max() - pad
        ? std::numeric_limits<std::uint64_t>::max() : range.end + pad;
    return Range(begin, end);
}

inline bool overlaps_at_granule(const Place& left, const Place& right, std::uint64_t granule) {
    left.validate();
    right.validate();
    if (left.store != right.store) return false;
    if (left.epoch != right.epoch)
        throw std::invalid_argument(
            "cannot compare DOP places from different storage epochs");
    if (!same_or_whole(left.archetype, right.archetype)) return false;
    if (!same_or_whole(left.component, right.component)) return false;
    if (!same_or_whole(left.field, right.field)) return false;
    return !left.rows || !right.rows
        || widen_to_granule(*left.rows, granule).overlaps(widen_to_granule(*right.rows, granule));
}

// Logical aliasing: do the two places name any common cell?
inline bool overlaps(const Place& left, const Place& right) {
    return overlaps_at_granule(left, right, 1);
}

inline bool conflicts(const Loan& left, const Loan& right) {
    left.validate();
    right.validate();
    const bool writes = left.access == Access::Write || right.access == Access::Write;
    const auto granule = writes ? std::max(left.place.granule, right.place.granule) : std::uint64_t{1};
    // Overlap runs first so stale epochs fail closed even for read/read.
    return overlaps_at_granule(left.place, right.place, granule) && writes;
}

inline bool can_run_concurrently(const std::vector<Loan>& left,
                                 const std::vector<Loan>& right) {
    for (const auto& loan : left) loan.validate();
    for (const auto& loan : right) loan.validate();
    for (const auto& a : left)
        for (const auto& b : right)
            if (conflicts(a, b)) return false;
    return true;
}

inline void require_internally_valid(const std::vector<Loan>& loans) {
    for (const auto& loan : loans) loan.validate();
    for (std::size_t i = 0; i < loans.size(); ++i)
        for (std::size_t j = i + 1; j < loans.size(); ++j)
            if (conflicts(loans[i], loans[j]))
                throw std::invalid_argument("conflicting DOP logical loans");
}

} // namespace oreslang::types::dop_access

