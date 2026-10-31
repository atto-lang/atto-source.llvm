#include "oreslang/types/NominalEnumOverloadPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "nominal enum overload check failed: %s at %s:%d\n", \
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
    const std::unordered_set<std::string> eligibleIds{
        "unit::Color", "unit::Mode", "other::Color"
    };
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
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}}, eligibleIds);
    Signature mode = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}}, eligibleIds);
    Signature sameColor = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}}, eligibleIds);
    Signature diffPrimitive = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "string"}}, eligibleIds);
    Signature arity1 = signature("go", {{Kind::Enum, "unit::Color"}}, eligibleIds);

    REQUIRE(compare(color, mode) == PairStatus::DistinctEnums);
    REQUIRE(compare(color, sameColor) == PairStatus::Same);
    REQUIRE(compare(color, diffPrimitive) == PairStatus::InvalidSameArity);
    REQUIRE(compare(color, arity1) == PairStatus::DistinctArity);
    REQUIRE(select({color, mode}, {{0, "unit::Mode"}}) == 1);
    REQUIRE(select({color, mode}, {{0, "unit::Color"}}) == 0);

    Signature twoColors = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Enum, "unit::Color"}}, eligibleIds);
    Signature twoModes = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Enum, "unit::Mode"}}, eligibleIds);
    rejects([&] { (void)select({twoColors, twoModes}, {{0, "unit::Color"}}); });
    rejects([&] { (void)select({twoColors, twoModes}, {{1, "unit::Color"}}); });
    rejects([&] { (void)select({twoColors, twoModes},
                               {{0, "unit::Color"}, {1, "unit::Mode"}}); });
    REQUIRE(select({twoColors, twoModes},
                   {{0, "unit::Color"}, {1, "unit::Color"}}) == 0);
    rejects([&] { (void)select({color, mode},
                               {{0, "unit::Mode"}, {1, "unit::Mode"}}); });
    Signature upper = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "String"}}, eligibleIds);
    REQUIRE(compare(color, upper) == PairStatus::InvalidSameArity);
    Signature lowercase = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "string"}}, eligibleIds);
    REQUIRE(compare(upper, lowercase) == PairStatus::Same);

    std::vector<std::pair<Kind, std::string>> tooManyParameters(
        MaxOverloadArity + 1, {Kind::Enum, "unit::Color"});
    rejects([&] { (void)signature("go", tooManyParameters, eligibleIds); });
    std::vector<Signature> tooManyCandidates(MaxSameArityCandidates + 1, color);
    rejects([&] { (void)select(tooManyCandidates, {{0, "unit::Color"}}); });

    rejects([&] { (void)select({color, mode}, {}); });
    rejects([&] { (void)select({color, mode}, {{0, "unknown::Color"}}); });
    rejects([&] { (void)select({color, mode}, {{2, "unit::Color"}}); });
    rejects([&] { (void)select({color, sameColor}, {{0, "unit::Color"}}); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "Unknown"}}, eligibleIds); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "{a:int}"}}, eligibleIds); });
    rejects([&] { (void)signature("go", {{Kind::Enum, ""}}, eligibleIds); });
    rejects([&] { (void)compare(color, signature("other", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}}, eligibleIds)); });

    rejects([&] { (void)signature("go", {
        {Kind::Enum, "attacker::Fake"}, {Kind::Primitive, "int"}
    }, eligibleIds); });
    // Even equal-shaped enums in different modules are distinct symbols.
    Signature imported = signature("go", {
        {Kind::Enum, "other::Color"}, {Kind::Primitive, "int"}}, eligibleIds);
    REQUIRE(compare(color, imported) == PairStatus::DistinctEnums);

    // Release-active input admission: enum payload layouts can arrive as
    // integer-cast/malformed metadata, not only compiler-created enum values.
    REQUIRE(!eligibleEnum({{static_cast<PayloadLayout>(255), {"int"}}}));
    REQUIRE(!eligibleEnum({{static_cast<PayloadLayout>(-1), {"int"}}}));
    REQUIRE(!eligibleEnum(std::vector<Variant>(
        MaxEnumCases + 1, Variant{PayloadLayout::Unit, {}})));
    REQUIRE(eligibleEnum(std::vector<Variant>(
        MaxEnumCases, Variant{PayloadLayout::Unit, {}})));
    REQUIRE(!eligibleEnum({{PayloadLayout::PrimitiveTuple,
        std::vector<std::string>(MaxEnumPayloadFields + 1, "int")}}));
    REQUIRE(eligibleEnum({{PayloadLayout::PrimitiveTuple,
        std::vector<std::string>(MaxEnumPayloadFields, "int")}}));

    // signature() and public aggregate-based compare()/select() must all
    // refuse malformed kind, identity and over-budget signature shapes.
    const Kind unknownKind = static_cast<Kind>(255);
    rejects([&] { (void)signature("go", {{unknownKind, "unit::Color"}}, eligibleIds); });
    const Signature forgedKind{"go", {{unknownKind, "unit::Color"}}};
    const Signature forgedPrimitive{"go", {{Kind::Primitive, "UnregisteredType"}}};
    const Signature forgedEmptyId{"go", {{Kind::Enum, ""}}};
    const Signature forgedEmptyName{"", {{Kind::Enum, "unit::Color"}}};
    const Signature forgedOversize{"go",
        std::vector<Position>(MaxOverloadArity + 1,
            Position{Kind::Enum, "unit::Color"})};
    rejects([&] { (void)compare(forgedKind, color); });
    rejects([&] { (void)compare(forgedPrimitive, color); });
    rejects([&] { (void)compare(forgedEmptyId, color); });
    rejects([&] { (void)compare(forgedEmptyName, color); });
    rejects([&] { (void)compare(forgedOversize, arity1); });
    rejects([&] { (void)select({forgedKind}, {}); });
    rejects([&] { (void)select({forgedPrimitive}, {}); });
    rejects([&] { (void)select({forgedEmptyId}, {}); });
    rejects([&] { (void)select({forgedOversize}, {}); });
    rejects([&] { (void)select({color, forgedKind}, {{0, "unit::Color"}}); });

    // This deliberately exercises only compiler-side keys, NOT LLVM source
    // parsing, AOT vtables, actor dispatch or guest execution.
    return 0;
}
