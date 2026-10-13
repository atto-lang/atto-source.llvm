#include "oreslang/types/NominalEnumOverloadPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "nominal enum overload check failed: %s at %s:%d\\n", \
                     #expression, __FILE__, __LINE__); \
        std::abort(); \
    } \
} while (false)

namespace {
using namespace oreslang::types::overloads;

void rejects(const std::function<void()>& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected && "unsafe nominal enum overload must reject");
}
}

int main() {
    REQUIRE(eligibleEnum({
        {PayloadLayout::Unit, {}},
        {PayloadLayout::Primitive, {"int"}},
        {PayloadLayout::PrimitiveTuple, {"bool", "string"}}
    }));
    REQUIRE(!eligibleEnum({}));
    REQUIRE(!eligibleEnum({{PayloadLayout::Unsupported, {"int"}}}));
    REQUIRE(!eligibleEnum({{PayloadLayout::Primitive, {"Color"}}}));
    REQUIRE(!eligibleEnum({{PayloadLayout::PrimitiveTuple, {"int", "Any"}}}));
    REQUIRE(!eligibleEnum({{PayloadLayout::Unit, {"int"}}}));
    REQUIRE(!eligibleEnum({{PayloadLayout::Primitive, {"int", "bool"}}}));

    Signature color = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}});
    Signature mode = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}});
    Signature sameColor = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}});
    Signature diffPrimitive = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "string"}});
    Signature arity1 = signature("go", {{Kind::Enum, "unit::Color"}});

    REQUIRE(compare(color, mode) == PairStatus::DistinctEnums);
    REQUIRE(compare(color, sameColor) == PairStatus::Same);
    REQUIRE(compare(color, diffPrimitive) == PairStatus::InvalidSameArity);
    REQUIRE(compare(color, arity1) == PairStatus::DistinctArity);
    REQUIRE(select({color, mode}, {{0, "unit::Mode"}}) == 1);
    REQUIRE(select({color, mode}, {{0, "unit::Color"}}) == 0);

    rejects([&] { (void)select({color, mode}, {}); });
    rejects([&] { (void)select({color, mode}, {{0, "unknown::Color"}}); });
    rejects([&] { (void)select({color, mode}, {{2, "unit::Color"}}); });
    rejects([&] { (void)select({color, sameColor}, {{0, "unit::Color"}}); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "Unknown"}}); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "{a:int}"}}); });
    rejects([&] { (void)signature("go", {{Kind::Enum, ""}}); });
    rejects([&] { (void)compare(color, signature("other", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}})); });

    // Even equal-shaped enums in different modules are distinct symbols.
    Signature imported = signature("go", {
        {Kind::Enum, "other::Color"}, {Kind::Primitive, "int"}});
    REQUIRE(compare(color, imported) == PairStatus::DistinctEnums);

    // This deliberately exercises only compiler-side keys, NOT LLVM source
    // parsing, AOT vtables, actor dispatch or guest execution.
    return 0;
}
