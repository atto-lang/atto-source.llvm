#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types {

// Canonical closed structural source shapes. The kind is part of identity.
// This is not a nominal local type, an i64 ABI, or an LLVM layout license.
struct NativeAggregateShape {
    enum class Kind { ExactStruct, ReadonlyRecord };
    struct Field {
        std::string name;
        std::string type;
    };
    Kind kind = Kind::ExactStruct;
    std::vector<Field> fields;

    // Compare unsigned UTF-8 bytes, equivalent to Unicode scalar ordering
    // for strictly validated UTF-8 names. Never depend on signed char order.
    static bool field_name_less(const std::string& left,
                                const std::string& right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](char a, char b) {
                return static_cast<unsigned char>(a) <
                       static_cast<unsigned char>(b);
            });
    }

    static void validate_static_name(const std::string& name) {
        if (name.empty()) throw std::invalid_argument("AGGREGATE_SHAPE_EMPTY_FIELD");
        if (name.size() > 1024)
            throw std::invalid_argument("AGGREGATE_SHAPE_FIELD_NAME_BUDGET");
        std::size_t i = 0;
        while (i < name.size()) {
            const auto first = static_cast<unsigned char>(name[i]);
            std::uint32_t code = 0;
            std::size_t width = 0;
            if (first <= 0x7f) { code = first; width = 1; }
            else if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; width = 2; }
            else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; width = 3; }
            else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; width = 4; }
            else throw std::invalid_argument("AGGREGATE_SHAPE_INVALID_UTF8");
            if (width > name.size() - i)
                throw std::invalid_argument("AGGREGATE_SHAPE_INVALID_UTF8");
            for (std::size_t j = 1; j < width; ++j) {
                const auto byte = static_cast<unsigned char>(name[i + j]);
                if ((byte & 0xc0) != 0x80)
                    throw std::invalid_argument("AGGREGATE_SHAPE_INVALID_UTF8");
                code = (code << 6) | (byte & 0x3f);
            }
            if ((width == 2 && code < 0x80) ||
                    (width == 3 && code < 0x800) ||
                    (width == 4 && code < 0x10000) ||
                    (code >= 0xd800 && code <= 0xdfff) ||
                    code > 0x10ffff)
                throw std::invalid_argument("AGGREGATE_SHAPE_INVALID_UTF8");
            if (code < 0x20 || (code >= 0x7f && code <= 0x9f))
                throw std::invalid_argument("AGGREGATE_SHAPE_CONTROL_FIELD_NAME");
            i += width;
        }
    }

    void canonicalize() {
        if (fields.size() > 64)
            throw std::invalid_argument("AGGREGATE_SHAPE_FIELD_BUDGET: more than 64 fields");
        for (const auto& field : fields) {
            validate_static_name(field.name);
            // v1 only records scalar field identity. Nested/opaque type names
            // cannot be silently treated as i64 or legal LLVM field layouts.
            if (field.type != "int" && field.type != "bool" &&
                    field.type != "string")
                throw std::invalid_argument("AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE: " + field.type);
        }
        std::sort(fields.begin(), fields.end(),
                  [](const Field& a, const Field& b) { return field_name_less(a.name, b.name); });
        for (std::size_t i = 1; i < fields.size(); ++i)
            if (fields[i].name == fields[i - 1].name)
                throw std::invalid_argument("AGGREGATE_SHAPE_DUPLICATE_FIELD: " + fields[i].name);
    }

    bool same_type_as(const NativeAggregateShape& other) const {
        if (kind != other.kind || fields.size() != other.fields.size()) return false;
        for (std::size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name != other.fields[i].name ||
                    fields[i].type != other.fields[i].type)
                return false;
        return true;
    }

    bool permits_implicit_source(const NativeAggregateShape& source) const {
        // Readonly record is not a supertype of exact struct in this contract.
        return same_type_as(source);
    }
};
} // namespace oreslang::types
