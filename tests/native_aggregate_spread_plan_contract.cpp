#include "oreslang/types/NativeAggregateSpreadPlan.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <map>

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

    CHECK(denies([] {
        NativeAggregateSpreadPlan excessive(Kind::ReadonlyRecord);
        for (int i=0;i<1025;i++)
            excessive.add_checked_spread_projection(NativeAggregateShape{
                Kind::ReadonlyRecord, {}});
    }, "AGGREGATE_SPREAD_PLAN_BUDGET"));

    // U+E000 sorts before U+10000 by scalar value, unlike UTF-16 ordinal.
    NativeAggregateSpreadPlan unicode(Kind::ReadonlyRecord);
    unicode.add_named("\xF0\x90\x80\x80", "int");
    unicode.add_named("\xEE\x80\x80", "int");
    CHECK(unicode.result_shape().fields[0].name == "\xEE\x80\x80");

    NativeAggregateSpreadPlan empty(Kind::ReadonlyRecord);
    empty.add_checked_spread_projection(NativeAggregateShape{Kind::ExactStruct, {}});
    CHECK(empty.result_shape().fields.empty());
    CHECK(empty.source_elements() == 1);

    // Failure must leave all previously checked writes intact: duplicate
    // explicit fields must not partially mutate the inferred source plan.
    NativeAggregateSpreadPlan atomic(Kind::ReadonlyRecord);
    atomic.add_named("x", "int");
    const auto before = atomic.source_ordered_writes().size();
    CHECK(denies([&] { atomic.add_named("x", "bool"); },
                 "AGGREGATE_SPREAD_DUPLICATE_NAMED_FIELD"));
    CHECK(atomic.source_ordered_writes().size() == before);
    CHECK(atomic.source_elements() == 1);
    CHECK(atomic.result_shape().fields[0].type == "int");

    // Empty spread elements have zero field writes, but still consume the
    // lexical element budget. Permit exactly 1024 and reject element 1025.
    NativeAggregateSpreadPlan element_budget(Kind::ExactStruct);
    for (int i = 0; i < 1024; ++i)
        element_budget.add_checked_spread_projection(
            NativeAggregateShape{Kind::ReadonlyRecord, {}});
    CHECK(element_budget.source_elements() == 1024);
    CHECK(element_budget.source_ordered_writes().empty());
    CHECK(denies([&] {
        element_budget.add_checked_spread_projection(
            NativeAggregateShape{Kind::ReadonlyRecord, {}});
    }, "AGGREGATE_SPREAD_PLAN_BUDGET"));
    CHECK(element_budget.source_elements() == 1024);

    // Eight thousand one hundred ninety-two writes must be accepted exactly;
    // even an additional field from a valid row must fail before mutation.
    NativeAggregateShape wide{Kind::ReadonlyRecord, {}};
    for (int i = 0; i < 64; ++i)
        wide.fields.push_back({"f" + std::to_string(i), "int"});
    wide.canonicalize();
    NativeAggregateSpreadPlan write_budget(Kind::ReadonlyRecord);
    for (int i = 0; i < 128; ++i)
        write_budget.add_checked_spread_projection(wide);
    CHECK(write_budget.source_elements() == 128);
    CHECK(write_budget.source_ordered_writes().size() == 8192);
    CHECK(write_budget.result_shape().fields.size() == 64);
    CHECK(denies([&] { write_budget.add_named("another", "int"); },
                 "AGGREGATE_SPREAD_PLAN_BUDGET"));
    CHECK(write_budget.source_ordered_writes().size() == 8192);

    // Regress amortized source-order storage: exact-size reserve on each
    // named write copies O(n^2) field strings at the 1,024-element budget.
    // Geometric growth should require only a handful of vector reallocations.
    NativeAggregateSpreadPlan growth(Kind::ReadonlyRecord);
    NativeAggregateShape repeated_source{Kind::ExactStruct, {{"x", "int"}}};
    repeated_source.canonicalize();
    std::size_t capacity_changes = 0;
    std::size_t last_capacity = growth.source_ordered_writes().capacity();
    for (int i = 0; i < 1024; ++i) {
        growth.add_checked_spread_projection(repeated_source);
        const auto& writes = growth.source_ordered_writes();
        CHECK(writes.size() == static_cast<std::size_t>(i + 1));
        CHECK(writes.capacity() >= writes.size());
        // C++ only guarantees capacity >= reserve request, not an upper
        // bound: do not assert a nonportable maximum capacity here.
        if (writes.capacity() != last_capacity) {
            CHECK(writes.capacity() > last_capacity);
            ++capacity_changes;
            last_capacity = writes.capacity();
        }
    }
    CHECK(growth.source_elements() == 1024);
    CHECK(capacity_changes >= 1 && capacity_changes <= 12);
    CHECK(growth.result_shape().fields.size() == 1);
    CHECK(denies([&] { growth.add_checked_spread_projection(repeated_source); },
                 "AGGREGATE_SPREAD_PLAN_BUDGET"));
    CHECK(growth.source_elements() == 1024);
    CHECK(growth.source_ordered_writes().size() == 1024);

    // UTF-8 validation is byte-based and rejects embedded NUL and truncated
    // multibyte keys instead of treating them as distinct static field names.
    CHECK(denies([] {
        NativeAggregateSpreadPlan bad(Kind::ReadonlyRecord);
        bad.add_named(std::string("a\0b", 3), "int");
    }, "AGGREGATE_SHAPE_CONTROL_FIELD_NAME"));
    CHECK(denies([] {
        NativeAggregateSpreadPlan bad(Kind::ReadonlyRecord);
        bad.add_named(std::string("\xE2\x82", 2), "int");
    }, "AGGREGATE_SHAPE_INVALID_UTF8"));

    // Deterministic property oracle: independently apply every source-ordered
    // write and check the inferred type row against its final overwrite map.
    // The corpus deliberately mixes two spread sources, different scalar
    // field types and explicitly named fields without relying on randomness.
    for (unsigned seed = 1; seed <= 512; ++seed) {
        NativeAggregateSpreadPlan checked(Kind::ReadonlyRecord);
        std::map<std::string, std::string> expected;
        unsigned state = seed * 1664525u + 1013904223u;
        const auto next_word = [&]() -> unsigned {
            state = state * 1664525u + 1013904223u;
            return state;
        };
        const unsigned elements = 1 + (seed % 13);
        std::size_t expected_writes = 0;
        for (unsigned i = 0; i < elements; ++i) {
            if ((next_word() & 3u) == 0u) {
                const std::string key = "explicit_" + std::to_string(i);
                const std::string type = (next_word() & 1u) ? "bool" : "int";
                checked.add_named(key, type);
                expected[key] = type;
                ++expected_writes;
            } else {
                NativeAggregateShape source{Kind::ExactStruct, {}};
                const unsigned field_count = next_word() % 7u;
                for (unsigned j = 0; j < field_count; ++j) {
                    // Keys deliberately overlap across earlier spread sources.
                    const auto type = (next_word() & 1u) ? "int" : "bool";
                    source.fields.push_back({"spread_" + std::to_string(j), type});
                }
                source.canonicalize();
                const auto frozen = source;
                checked.add_checked_spread_projection(source);
                CHECK(source.same_type_as(frozen));
                for (const auto& field : source.fields) {
                    expected[field.name] = field.type;
                    ++expected_writes;
                }
            }
        }
        CHECK(checked.source_elements() == elements);
        CHECK(checked.source_ordered_writes().size() == expected_writes);
        const auto inferred = checked.result_shape();
        CHECK(inferred.kind == Kind::ReadonlyRecord);
        CHECK(inferred.fields.size() == expected.size());
        for (const auto& field : inferred.fields) {
            const auto it = expected.find(field.name);
            CHECK(it != expected.end() && it->second == field.type);
        }
        CHECK(inferred.same_type_as(checked.result_shape()));
    }
    return 0;
}
