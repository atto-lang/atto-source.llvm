#pragma once

#include "oreslang/runtime/DopHeterogeneousLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::dop::physical_v1 {

// Typed compiler-facing scalar-record address IR. This bridge reuses native
// HeterogeneousPlan's checked physical segments and exposes cell addresses.
// It does NOT yet parse or lower AttoLang inverted source syntax into LLVM IR.
enum class LayoutKind { aos, soa, aosoa, soaos };
struct TypedField {
    std::string name;
    std::uint32_t bytes;
    std::uint32_t alignment;
};
struct PhysicalCell {
    std::uint32_t segment;
    std::uint64_t relative_offset;
    std::uint64_t absolute_offset;
};

class TypedPhysicalPlan final {
public:
    TypedPhysicalPlan(LayoutKind kind, std::uint64_t rows,
                      std::vector<TypedField> fields, std::uint32_t lanes = 1,
                      std::vector<std::vector<std::uint32_t>> groups = {},
                      std::uint64_t max_bytes = static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
        : fields_(std::move(fields)),
          underlying_(make_plan(kind, rows, fields_, lanes, groups, max_bytes)) {}

    const HeterogeneousPlan& substrate() const noexcept { return underlying_; }
    std::uint64_t physical_bytes() const noexcept { return underlying_.physical_bytes(); }
    const std::vector<SegmentSpec>& segments() const noexcept {
        return underlying_.segments();
    }

    PhysicalCell lower_cell_address(std::uint64_t row, std::uint32_t field) const {
        const auto loc = underlying_.address(row, field);
        const auto& s = underlying_.segments().at(loc.segment);
        // Ensure the entire typed scalar fits the source layout segment.
        if (loc.byte_offset > s.bytes || fields_.at(field).bytes > s.bytes - loc.byte_offset)
            throw std::out_of_range("typed physical scalar escapes segment");
        return {loc.segment, loc.byte_offset, underlying_.absolute_address(row, field)};
    }

private:
    // Strict Unicode scalar names encoded as UTF-8. No normalization, no NUL,
    // no overlong encodings, surrogates or out-of-range scalar values.
    // This is metadata validation only, not source-language identifier syntax.
    static bool valid_name(const std::string& value) {
        if (value.empty() || value.size() > 1024) return false;
        for (std::size_t i = 0; i < value.size();) {
            const auto lead = static_cast<unsigned char>(value[i]);
            if (lead == 0) return false;
            if (lead < 0x80) { ++i; continue; }
            std::size_t trailing = 0;
            std::uint32_t scalar = 0;
            std::uint32_t minimum = 0;
            if (lead >= 0xC2 && lead <= 0xDF) {
                trailing = 1; scalar = lead & 0x1F; minimum = 0x80;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                trailing = 2; scalar = lead & 0x0F; minimum = 0x800;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                trailing = 3; scalar = lead & 0x07; minimum = 0x10000;
            } else {
                return false;
            }
            if (trailing >= value.size() - i) return false;
            for (std::size_t j = 1; j <= trailing; ++j) {
                const auto part = static_cast<unsigned char>(value[i + j]);
                if (part < 0x80 || part > 0xBF) return false;
                scalar = (scalar << 6) | (part & 0x3F);
            }
            if (scalar < minimum || scalar > 0x10FFFF ||
                (scalar >= 0xD800 && scalar <= 0xDFFF)) return false;
            i += trailing + 1;
        }
        return true;
    }

    static HeterogeneousLayout translate(LayoutKind kind) {
        switch (kind) {
        case LayoutKind::aos: return HeterogeneousLayout::AoS;
        case LayoutKind::soa: return HeterogeneousLayout::SoA;
        case LayoutKind::aosoa: return HeterogeneousLayout::AoSoA;
        case LayoutKind::soaos: return HeterogeneousLayout::SoAoS;
        }
        throw std::invalid_argument("unknown physical layout directive");
    }

    static HeterogeneousPlan make_plan(
            LayoutKind kind, std::uint64_t rows, const std::vector<TypedField>& fields,
            std::uint32_t lanes, const std::vector<std::vector<std::uint32_t>>& groups,
            std::uint64_t max_bytes) {
        constexpr auto max_signed = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        if (rows > max_signed || max_bytes > max_signed ||
            fields.empty() || fields.size() > 64)
            throw std::invalid_argument("typed physical layout dimension outside v1 bounds");
        std::unordered_set<std::string> names;
        std::vector<FieldSpec> specs;
        specs.reserve(fields.size());
        for (const auto& field : fields) {
            if (!valid_name(field.name) || !names.insert(field.name).second)
                throw std::invalid_argument("invalid or duplicate typed field name");
            specs.push_back({field.bytes, field.alignment});
        }
        const auto layout = translate(kind);
        // Quota includes alignment gaps between separately grouped segments.
        return HeterogeneousPlan(layout, rows, specs, lanes, groups,
                                 max_signed, max_bytes);
    }

    std::vector<TypedField> fields_;
    HeterogeneousPlan underlying_;
};
} // namespace oreslang::dop::physical_v1
