#pragma once

#include "oreslang/runtime/DopLayout.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Heterogeneous, byte-accurate and allocation-free-at-address-time layout plan.
// Reference substrate for source-lowered inverted classes; NOT a source compiler.
// Grouped SoAoS is one compact AoS record *per group*, stored in a distinct
// array; unlike AoSoA, it never uses scalar-column tiles.
enum class HeterogeneousLayout { AoS, SoA, AoSoA, SoAoS };
struct FieldSpec {
    std::uint32_t bytes;
    std::uint32_t alignment;
};
struct CellAddress {
    std::uint32_t segment;
    std::uint64_t byte_offset;
};
struct SegmentSpec {
    std::uint32_t alignment;
    std::uint64_t bytes;
    std::uint64_t stride;
    std::uint64_t base = 0;  // aligned location in a single actor-owned allocation
};

class HeterogeneousPlan final {
public:
    HeterogeneousPlan(HeterogeneousLayout layout,
                      std::uint64_t rows,
                      const std::vector<FieldSpec>& fields,
                      std::uint32_t lanes = 1,
                      const std::vector<std::vector<std::uint32_t>>& groups = {},
                      std::uint64_t max_segment_bytes = std::numeric_limits<std::uint64_t>::max(),
                      std::uint64_t max_total_bytes = std::numeric_limits<std::uint64_t>::max())
        : layout_(layout), rows_(rows), fields_(fields), lanes_(lanes) {
        if (fields.empty() || fields.size() > 1024)
            throw std::invalid_argument("heterogeneous layout requires 1..1024 fields");
        for (const auto& field : fields) {
            if (!power_of_two(field.bytes) || !power_of_two(field.alignment) ||
                field.bytes > field.alignment || field.alignment > 4096)
                throw std::invalid_argument("invalid heterogeneous scalar width/alignment");
        }
        if (layout_ != HeterogeneousLayout::AoSoA && lanes != 1)
            throw std::invalid_argument("only AoSoA accepts multiple lanes");
        if (!power_of_two(lanes) || lanes > 1024)
            throw std::invalid_argument("invalid AoSoA tile width");
        if (layout_ != HeterogeneousLayout::SoAoS && !groups.empty())
            throw std::invalid_argument("field groups are exclusive to SoAoS");
        if (layout_ == HeterogeneousLayout::SoAoS && groups.empty())
            throw std::invalid_argument("SoAoS requires an explicit nonempty partition");

        locations_.resize(fields.size());
        switch (layout_) {
        case HeterogeneousLayout::AoS: {
            auto row = record_shape(sequence_indices());
            segments_.push_back({row.alignment, checked_mul(rows_, row.stride), row.stride});
            for (std::size_t i = 0; i < fields.size(); ++i)
                locations_[i] = {0, row.offsets[i], row.stride};
            break;
        }
        case HeterogeneousLayout::SoA:
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto& field = fields_[i];
                const auto stride = align_up(field.bytes, field.alignment);
                segments_.push_back({field.alignment, checked_mul(rows_, stride), stride});
                locations_[i] = {static_cast<std::uint32_t>(i), 0, stride};
            }
            break;
        case HeterogeneousLayout::AoSoA: {
            std::uint64_t cursor = 0;
            std::uint32_t alignment = 1;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto& field = fields_[i];
                cursor = align_up(cursor, field.alignment);
                const auto stride = align_up(field.bytes, field.alignment);
                locations_[i] = {0, cursor, stride};
                cursor = checked_add(cursor, checked_mul(stride, lanes_));
                if (field.alignment > alignment) alignment = field.alignment;
            }
            const auto stride = align_up(cursor, alignment);
            const auto tiles = checked_add(rows_ / lanes_, rows_ % lanes_ != 0 ? 1 : 0);
            segments_.push_back({alignment, checked_mul(tiles, stride), stride});
            break;
        }
        case HeterogeneousLayout::SoAoS: {
            std::vector<bool> seen(fields.size(), false);
            std::size_t accounted = 0;
            for (const auto& group : groups) {
                if (group.empty()) throw std::invalid_argument("SoAoS field group is empty");
                for (const auto index : group) {
                    if (index >= fields.size() || seen[index])
                        throw std::invalid_argument("SoAoS field groups are not a partition");
                    seen[index] = true;
                    ++accounted;
                }
                auto record = record_shape(group);
                const auto segment = static_cast<std::uint32_t>(segments_.size());
                segments_.push_back({record.alignment,
                                     checked_mul(rows_, record.stride), record.stride});
                for (std::size_t i = 0; i < group.size(); ++i)
                    locations_[group[i]] = {segment, record.offsets[i], record.stride};
            }
            if (accounted != fields.size())
                throw std::invalid_argument("SoAoS field groups do not cover all fields");
            break;
        }
        default:
            throw std::invalid_argument("unknown heterogeneous layout");
        }
        std::uint64_t payload = 0;
        std::uint64_t physical = 0;
        for (auto& segment : segments_) {
            if (segment.bytes > max_segment_bytes)
                throw std::length_error("heterogeneous segment exceeds physical quota");
            // All segments inhabit ONE actor-owned allocation. Charge the
            // base-alignment holes, not only the sum of field payloads.
            physical = align_up(physical, segment.alignment);
            segment.base = physical;
            physical = checked_add(physical, segment.bytes);
            payload = checked_add(payload, segment.bytes);
            if (physical > max_total_bytes)
                throw std::length_error("heterogeneous layout exceeds actor quota");
        }
        total_bytes_ = payload;
        physical_bytes_ = physical;
    }

    HeterogeneousLayout layout() const noexcept { return layout_; }
    std::uint64_t rows() const noexcept { return rows_; }
    std::uint32_t field_count() const noexcept { return static_cast<std::uint32_t>(fields_.size()); }
    std::uint32_t lanes() const noexcept { return lanes_; }
    std::uint64_t total_bytes() const noexcept { return total_bytes_; }
    const std::vector<SegmentSpec>& segments() const noexcept { return segments_; }
    std::uint64_t physical_bytes() const noexcept { return physical_bytes_; }

    std::uint64_t absolute_address(std::uint64_t row, std::uint32_t field) const {
        const auto location = address(row, field);
        return checked_add(segments_.at(location.segment).base, location.byte_offset);
    }

    // Checked, non-throwing lookup for admitted RT kernels. The plan and its
    // metadata must be constructed before the critical execution window.
    // It returns an OFFSET, never a pointer or an authority to cross actors.
    // No vectors grow and no exception path is entered on invalid indices.
    bool try_absolute_address(std::uint64_t row,
                              std::uint32_t field,
                              std::uint64_t* out) const noexcept {
        if (!out || row >= rows_ || field >= fields_.size()) return false;
        const auto& loc = locations_[field];
        if (loc.segment >= segments_.size()) return false;
        const auto& segment = segments_[loc.segment];
        constexpr auto max = std::numeric_limits<std::uint64_t>::max();
        const auto add = [](std::uint64_t a, std::uint64_t b,
                            std::uint64_t* result) noexcept {
            if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
            *result = a + b;
            return true;
        };
        const auto mul = [](std::uint64_t a, std::uint64_t b,
                            std::uint64_t* result) noexcept {
            if (a && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
            *result = a * b;
            return true;
        };
        std::uint64_t base = 0, lane_bytes = 0, relative = 0;
        if (layout_ == HeterogeneousLayout::AoSoA) {
            if (!mul(row / lanes_, segment.stride, &base) ||
                !mul(row % lanes_, loc.row_stride, &lane_bytes) ||
                !add(base, loc.offset, &relative) ||
                !add(relative, lane_bytes, &relative)) return false;
        } else {
            if (!mul(row, loc.row_stride, &base) ||
                !add(base, loc.offset, &relative)) return false;
        }
        if (relative > segment.bytes ||
            fields_[field].bytes > segment.bytes - relative ||
            segment.base > max - relative) return false;
        *out = segment.base + relative;
        return true;
    }

    CellAddress address(std::uint64_t row, std::uint32_t field) const {
        if (row >= rows_ || field >= fields_.size())
            throw std::out_of_range("heterogeneous row/field out of bounds");
        const auto& loc = locations_[field];
        if (layout_ == HeterogeneousLayout::AoSoA) {
            const auto tile = row / lanes_;
            const auto lane = row % lanes_;
            return {loc.segment, checked_add(checked_add(
                checked_mul(tile, segments_[0].stride), loc.offset),
                checked_mul(lane, loc.row_stride))};
        }
        return {loc.segment, checked_add(checked_mul(row, loc.row_stride), loc.offset)};
    }

private:
    struct FieldLocator {
        std::uint32_t segment;
        std::uint64_t offset;
        std::uint64_t row_stride;
    };
    struct RecordShape {
        std::uint32_t alignment;
        std::uint64_t stride;
        std::vector<std::uint64_t> offsets;
    };
    std::vector<std::uint32_t> sequence_indices() const {
        std::vector<std::uint32_t> values;
        values.reserve(fields_.size());
        for (std::uint32_t i = 0; i < fields_.size(); ++i) values.push_back(i);
        return values;
    }
    RecordShape record_shape(const std::vector<std::uint32_t>& indices) const {
        RecordShape result{1, 0, {}};
        result.offsets.reserve(indices.size());
        for (const auto index : indices) {
            const auto& f = fields_.at(index);
            result.stride = align_up(result.stride, f.alignment);
            result.offsets.push_back(result.stride);
            result.stride = checked_add(result.stride, f.bytes);
            if (f.alignment > result.alignment) result.alignment = f.alignment;
        }
        result.stride = align_up(result.stride, result.alignment);
        return result;
    }
    HeterogeneousLayout layout_;
    std::uint64_t rows_;
    std::vector<FieldSpec> fields_;
    std::uint32_t lanes_;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t physical_bytes_ = 0;
    std::vector<FieldLocator> locations_;
    std::vector<SegmentSpec> segments_;
};

} // namespace oreslang::dop
