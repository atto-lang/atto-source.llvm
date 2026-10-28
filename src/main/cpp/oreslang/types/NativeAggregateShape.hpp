#pragma once

#include <algorithm>
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

    void canonicalize() {
        if (fields.size() > 64)
            throw std::invalid_argument("AGGREGATE_SHAPE_FIELD_BUDGET: more than 64 fields");
        for (const auto& field : fields) {
            if (field.name.empty())
                throw std::invalid_argument("AGGREGATE_SHAPE_EMPTY_FIELD");
            // v1 only records scalar field identity. Nested/opaque type names
            // cannot be silently treated as i64 or legal LLVM field layouts.
            if (field.type != "int" && field.type != "bool" &&
                    field.type != "string")
                throw std::invalid_argument("AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE: " + field.type);
        }
        std::sort(fields.begin(), fields.end(),
                  [](const Field& a, const Field& b) { return a.name < b.name; });
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
