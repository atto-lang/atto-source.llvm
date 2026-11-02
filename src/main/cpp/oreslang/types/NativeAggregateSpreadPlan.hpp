#pragma once

#include "oreslang/types/NativeAggregateShape.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types {

// Compile-time closed-row type inference and source-ordered write planning.
// This is not a runtime aggregate ABI, ownership capability or permission to
// lower a source expression. Borrow/copy/actor proofs remain mandatory.
class NativeAggregateSpreadPlan {
public:
    using Kind = NativeAggregateShape::Kind;
    using Field = NativeAggregateShape::Field;

    struct Write {
        std::size_t source_element;
        std::string field;
        std::string type;
        bool from_spread;
    };

    explicit NativeAggregateSpreadPlan(Kind result_kind)
        : result_kind_(result_kind) {}

    // One syntactically named field, evaluated once before later elements.
    void add_named(std::string name, std::string type) {
        require_budget(1);
        NativeAggregateShape candidate{result_kind_, {{name, type}}};
        candidate.canonicalize(); // rejects invalid and unsupported shapes
        if (!explicit_names_.insert(name).second)
            throw std::invalid_argument("AGGREGATE_SPREAD_DUPLICATE_NAMED_FIELD: " + name);
        writes_.push_back(Write{next_element_++, std::move(name),
                                std::move(type), false});
    }

    // Source must already be a checked, accessible and closed static projection.
    // Never enumerate a wider runtime object or implicitly consume its owner.
    void add_checked_spread_projection(NativeAggregateShape source) {
        source.canonicalize();
        require_budget(source.fields.size());
        const std::size_t element = next_element_++;
        for (const auto& field : source.fields)
            writes_.push_back(Write{element, field.name, field.type, true});
    }

    // The output is a fresh row: rightmost write wins, including a new type.
    // Neither the source row nor prior generated rows are mutated.
    NativeAggregateShape result_shape() const {
        std::map<std::string, std::string> row;
        for (const Write& write : writes_)
            row[write.field] = write.type;
        if (row.size() > 64)
            throw std::invalid_argument("AGGREGATE_SHAPE_FIELD_BUDGET: more than 64 final fields");
        NativeAggregateShape result{result_kind_, {}};
        for (const auto& [name,type] : row)
            result.fields.push_back(Field{name,type});
        result.canonicalize();
        return result;
    }

    const std::vector<Write>& source_ordered_writes() const noexcept {
        return writes_;
    }

    std::size_t source_elements() const noexcept { return next_element_; }

private:
    void require_budget(std::size_t additional_writes) const {
        if (next_element_ >= 1024 || additional_writes > 8192 - writes_.size())
            throw std::invalid_argument("AGGREGATE_SPREAD_PLAN_BUDGET");
    }
    Kind result_kind_;
    std::size_t next_element_ = 0;
    std::set<std::string> explicit_names_;
    std::vector<Write> writes_;
};
} // namespace oreslang::types
