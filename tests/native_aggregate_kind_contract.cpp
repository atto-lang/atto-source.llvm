#include "oreslang/types/NativeAggregateShape.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using oreslang::types::NativeAggregateShape;
using Kind = NativeAggregateShape::Kind;

// Never erase test checks in Release/-DNDEBUG builds.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "aggregate shape check failed at line %d: %s\n", \
                     __LINE__, #condition); \
        return 1; \
    } \
} while (false)

static bool rejects(NativeAggregateShape shape, const char* diagnostic) {
    try {
        shape.canonicalize();
    } catch (const std::invalid_argument& ex) {
        return std::string(ex.what()).find(diagnostic) != std::string::npos;
    }
    return false;
}

int main() {
    NativeAggregateShape exact{Kind::ExactStruct,
        {{"foo", "int"}, {"bar", "bool"}}};
    NativeAggregateShape record{Kind::ReadonlyRecord,
        {{"bar", "bool"}, {"foo", "int"}}};
    NativeAggregateShape reordered{Kind::ExactStruct,
        {{"bar", "bool"}, {"foo", "int"}}};
    exact.canonicalize();
    record.canonicalize();
    reordered.canonicalize();
    CHECK(exact.same_type_as(reordered));
    CHECK(reordered.same_type_as(exact));
    CHECK(exact.permits_implicit_source(reordered));
    CHECK(!exact.same_type_as(record));
    CHECK(!record.same_type_as(exact));
    CHECK(!record.permits_implicit_source(exact));
    CHECK(!exact.permits_implicit_source(record));

    NativeAggregateShape different_field{Kind::ExactStruct,
        {{"bar", "bool"}, {"other", "int"}}};
    NativeAggregateShape different_type{Kind::ExactStruct,
        {{"bar", "int"}, {"foo", "int"}}};
    different_field.canonicalize();
    different_type.canonicalize();
    CHECK(!exact.same_type_as(different_field));
    CHECK(!exact.same_type_as(different_type));
    CHECK(!exact.permits_implicit_source(different_field));

    NativeAggregateShape empty_struct{Kind::ExactStruct, {}};
    NativeAggregateShape empty_record{Kind::ReadonlyRecord, {}};
    empty_struct.canonicalize();
    empty_record.canonicalize();
    CHECK(empty_struct.same_type_as(empty_struct));
    CHECK(!empty_struct.same_type_as(empty_record));

    NativeAggregateShape max_fields{Kind::ExactStruct, {}};
    for (int i = 0; i < 64; ++i)
        max_fields.fields.push_back({"field" + std::to_string(i), "int"});
    max_fields.canonicalize();
    CHECK(max_fields.fields.size() == 64);
    CHECK(max_fields.fields.front().name == "field0");
    CHECK(max_fields.same_type_as(max_fields));
    max_fields.canonicalize(); // Idempotence of canonical ordering.
    CHECK(max_fields.same_type_as(max_fields));
    auto too_many = max_fields;
    too_many.fields.push_back({"field64", "int"});
    CHECK(rejects(too_many, "AGGREGATE_SHAPE_FIELD_BUDGET"));

    CHECK(rejects({Kind::ExactStruct, {{"foo", "int"}, {"foo", "int"}}},
                  "AGGREGATE_SHAPE_DUPLICATE_FIELD"));
    CHECK(rejects({Kind::ReadonlyRecord, {{"foo", "int"}, {"foo", "bool"}}},
                  "AGGREGATE_SHAPE_DUPLICATE_FIELD"));
    CHECK(rejects({Kind::ReadonlyRecord, {{"ptr", "Pointer<Thing>"}}},
                  "AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE"));
    CHECK(rejects({Kind::ExactStruct, {{"nested", "record"}}},
                  "AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE"));
    CHECK(rejects({Kind::ExactStruct, {{"", "int"}}},
                  "AGGREGATE_SHAPE_EMPTY_FIELD"));
    return 0;
}
