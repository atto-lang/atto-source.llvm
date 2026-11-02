#include "oreslang/types/NativeAggregateSpreadPlan.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using oreslang::types::NativeAggregateSpreadPlan;
using oreslang::types::NativeAggregateShape;
using Kind = NativeAggregateShape::Kind;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "aggregate-spread plan failed: %s at %d\n", #x, __LINE__); return 1; } } while (false)

template <typename F>
bool denies(F&& run, const char* expected) {
    try { run(); }
    catch (const std::invalid_argument& failure) {
        return std::string(failure.what()).find(expected) != std::string::npos;
    }
    return false;
}

int main() {
    NativeAggregateShape original{Kind::ExactStruct, {{"foo", "int"}}};
    original.canonicalize();
    NativeAggregateSpreadPlan extend(Kind::ExactStruct);
    extend.add_checked_spread_projection(original);
    extend.add_named("bar", "string");
    const auto next = extend.result_shape();
    CHECK(next.kind == Kind::ExactStruct && next.fields.size() == 2);
    CHECK(next.fields[0].name == "bar" && next.fields[0].type == "string");
    CHECK(next.fields[1].name == "foo" && next.fields[1].type == "int");
    CHECK(original.fields.size() == 1 && original.fields[0].type == "int");
    CHECK(extend.source_elements() == 2 && extend.source_ordered_writes().size() == 2);
    CHECK(extend.source_ordered_writes()[0].from_spread);
    CHECK(extend.source_ordered_writes()[1].source_element == 1);

    // Explicit construction of a readonly record is not implicit erasure.
    NativeAggregateSpreadPlan as_record(Kind::ReadonlyRecord);
    as_record.add_checked_spread_projection(next);
    as_record.add_named("foo", "bool");
    auto row = as_record.result_shape();
    CHECK(row.kind == Kind::ReadonlyRecord);
    CHECK(row.fields[1].name == "foo" && row.fields[1].type == "bool");
    CHECK(!row.same_type_as(next));

    // Rightmost spread overrides a named field without erasing its write.
    NativeAggregateSpreadPlan overrides(Kind::ReadonlyRecord);
    overrides.add_named("x", "string");
    overrides.add_checked_spread_projection(NativeAggregateShape{
        Kind::ReadonlyRecord, {{"y", "int"}, {"x", "bool"}}});
    overrides.add_named("z", "int");
    auto typed = overrides.result_shape();
    CHECK(typed.fields.size() == 3);
    CHECK(typed.fields[0].name == "x" && typed.fields[0].type == "bool");
    CHECK(overrides.source_ordered_writes().size() == 4);
    CHECK(overrides.source_ordered_writes()[0].field == "x");
    CHECK(overrides.source_ordered_writes()[1].field == "x");

    // Duplicate *named* fields reject even when an intervening spread exists.
    CHECK(denies([] {
        NativeAggregateSpreadPlan bad(Kind::ReadonlyRecord);
        bad.add_named("value", "int");
        bad.add_checked_spread_projection(NativeAggregateShape{
            Kind::ReadonlyRecord, {{"value", "string"}}});
        bad.add_named("value", "bool");
    }, "AGGREGATE_SPREAD_DUPLICATE_NAMED_FIELD"));

    CHECK(denies([] {
        NativeAggregateSpreadPlan bad(Kind::ExactStruct);
        bad.add_checked_spread_projection(NativeAggregateShape{
            Kind::ReadonlyRecord, {{"nested", "record"}}});
    }, "AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE"));

    CHECK(denies([] {
        NativeAggregateSpreadPlan bad(Kind::ReadonlyRecord);
        for (int i=0;i<65;i++)
            bad.add_named("field"+std::to_string(i), "int");
        (void)bad.result_shape();
    }, "AGGREGATE_SHAPE_FIELD_BUDGET"));

    NativeAggregateSpreadPlan empty(Kind::ReadonlyRecord);
    empty.add_checked_spread_projection(NativeAggregateShape{Kind::ExactStruct, {}});
    CHECK(empty.result_shape().fields.empty());
    CHECK(empty.source_elements() == 1);
    return 0;
}
