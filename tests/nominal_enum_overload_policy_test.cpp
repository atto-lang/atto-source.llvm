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
    REQUIRE(!eligibleEnum({{static_cast<PayloadLayout>(99), {"int"}}}));
    REQUIRE(!eligibleEnum({{static_cast<PayloadLayout>(255), {"int"}}}));

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

    // A caller cannot bypass the signature factory's static bounds by
    // constructing the exported aggregate directly.
    Signature excessive{"go", std::vector<Position>(
        MaxOverloadArity + 1, Position{Kind::Enum, "unit::Color"})};
    rejects([&] { (void)compare(excessive, arity1); });
    rejects([&] { (void)select({excessive}, {}); });
    Signature invalidPrimitive{"go", {{Kind::Primitive, "Any"}}};
    Signature unqualifiedEnum{"go", {{Kind::Enum, "Color"}}};
    Signature missingId{"go", {{Kind::Enum, ""}}};
    Signature unknownKind{"go", {{static_cast<Kind>(99), "unit::Color"}}};
    rejects([&] { (void)select({invalidPrimitive}, {}); });
    rejects([&] { (void)select({unqualifiedEnum}, {}); });
    rejects([&] { (void)select({missingId}, {}); });
    rejects([&] { (void)select({unknownKind}, {}); });
    // Public C++ aggregates cannot bypass bounded ABI identity admission,
    // even when no pairwise comparison would otherwise inspect the key.
    for (const std::string& malformed : std::vector<std::string>{
            "pkg/a.ores::0Color", "pkg/a.ores::Bad-Case",
            "pkg::::Color", "pkg/a.ores::Color\n",
            "pkg/a.ores::" + std::string(1024, 'A')}) {
        rejects([&] {
            (void)select({Signature{"go", {{Kind::Enum, malformed}}}}, {});
        });
    }
    rejects([&] {
        (void)select({Signature{"go", {{Kind::Primitive, "String"}}}}, {});
    });
    rejects([&] { (void)signature("go",
        {{static_cast<Kind>(99), "unit::Color"}}, eligibleIds); });


    rejects([&] { (void)select({color, mode}, {}); });
    rejects([&] { (void)select({color, mode}, {{0, "unknown::Color"}}); });
    rejects([&] { (void)select({color, mode}, {{2, "unit::Color"}}); });
    rejects([&] { (void)select({color, sameColor}, {{0, "unit::Color"}}); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "Unknown"}}, eligibleIds); });
    rejects([&] { (void)signature("go", {{Kind::Primitive, "{a:int}"}}, eligibleIds); });
    rejects([&] { (void)signature("go", {{Kind::Enum, ""}}, eligibleIds); });
    rejects([&] { (void)signature("go", {
        {static_cast<Kind>(255), "int"}}, eligibleIds); });
    rejects([&] { (void)compare(color, signature("other", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}}, eligibleIds)); });

    rejects([&] { (void)signature("go", {
        {Kind::Enum, "attacker::Fake"}, {Kind::Primitive, "int"}
    }, eligibleIds); });
    // Even equal-shaped enums in different modules are distinct symbols.
    Signature imported = signature("go", {
        {Kind::Enum, "other::Color"}, {Kind::Primitive, "int"}}, eligibleIds);
    REQUIRE(compare(color, imported) == PairStatus::DistinctEnums);

    // Full-vector static proof: a discriminator match must not allow a
    // different non-discriminator primitive argument to reach the call site.
    REQUIRE(selectResolved({color, mode},
                           {{Kind::Enum, "unit::Color"},
                            {Kind::Primitive, "int"}}, eligibleIds) == 0);
    REQUIRE(selectResolved({color, mode},
                           {{Kind::Enum, "unit::Mode"},
                            {Kind::Primitive, "int"}}, eligibleIds) == 1);
    rejects([&] { (void)selectResolved(
        {color, mode}, {{Kind::Enum, "unit::Color"},
                        {Kind::Primitive, "string"}}, eligibleIds); });
    rejects([&] { (void)selectResolved(
        {color, mode}, {{Kind::Enum, "unit::Color"},
                        {Kind::Primitive, "Unknown"}}, eligibleIds); });
    rejects([&] { (void)selectResolved(
        {color, mode}, {{Kind::Enum, "unit::Color"},
                        {Kind::Primitive, "f32"}}, eligibleIds); });
    rejects([&] { (void)selectResolved(
        {color, mode}, {{Kind::Enum, "attacker::Fake"},
                        {Kind::Primitive, "int"}}, eligibleIds); });
    rejects([&] { (void)selectResolved(
        {color, mode}, {{Kind::Enum, "unit::Color"}}, eligibleIds); });
    rejects([&] { (void)selectResolved(
        {color}, {{Kind::Enum, "unit::Color"},
                  {Kind::Primitive, "int"}}, eligibleIds); });

    Signature colorF64 = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "f64"}}, eligibleIds);
    Signature modeF64 = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "f64"}}, eligibleIds);
    REQUIRE(selectResolved({colorF64, modeF64},
                           {{Kind::Enum, "unit::Color"},
                            {Kind::Primitive, "f64"}}, eligibleIds) == 0);
    rejects([&] { (void)selectResolved(
        {colorF64, modeF64}, {{Kind::Enum, "unit::Color"},
                              {Kind::Primitive, "int"}}, eligibleIds); });

    // Public C++ aggregate constructors must not bypass the eligibility
    // registry, primitive canonicalization or maximum arity.
    Signature forgedEnum{"go", {{Kind::Enum, "attacker::Fake"},
                                 {Kind::Primitive, "int"}}};
    rejects([&] { (void)selectResolved(
        {forgedEnum, mode},
        {{Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}},
        eligibleIds); });
    Signature forgedPrimitive{"go", {{Kind::Enum, "unit::Color"},
                                      {Kind::Primitive, "Unknown"}}};
    rejects([&] { (void)selectResolved(
        {forgedPrimitive, mode},
        {{Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}},
        eligibleIds); });
    Signature uncanonical{"go", {{Kind::Enum, "unit::Color"},
                                 {Kind::Primitive, "String"}}};
    rejects([&] { (void)selectResolved(
        {uncanonical, mode},
        {{Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}},
        eligibleIds); });

    // A cast to the public C++ enum can carry an invalid discriminator
    // that resembles a primitive; the compiler cannot admit this as a slot.
    Signature invalidKind{"go", {
        {Kind::Enum, "unit::Color"},
        {static_cast<Kind>(255), "int"}
    }};
    rejects([&] { (void)selectResolved(
        {invalidKind, mode},
        {{Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}},
        eligibleIds); });

    // This deliberately exercises only compiler-side keys, NOT LLVM source
    // parsing, AOT vtables, actor dispatch or guest execution.
    return 0;
}
