#include "oreslang/types/NominalEnumOverloadPolicy.h"

#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace oreslang::types::overloads;

void rejects(const std::function<void()>& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected && "unsafe nominal enum overload must reject");
}
}

int main() {
    assert(eligibleEnum({
        {PayloadLayout::Unit, {}},
        {PayloadLayout::Primitive, {"int"}},
        {PayloadLayout::PrimitiveTuple, {"bool", "string"}}
    }));
    assert(!eligibleEnum({}));
    assert(!eligibleEnum({{PayloadLayout::Unsupported, {"int"}}}));
    assert(!eligibleEnum({{PayloadLayout::Primitive, {"Color"}}}));
    assert(!eligibleEnum({{PayloadLayout::PrimitiveTuple, {"int", "Any"}}}));
    assert(!eligibleEnum({{PayloadLayout::Unit, {"int"}}}));
    assert(!eligibleEnum({{PayloadLayout::Primitive, {"int", "bool"}}}));

    Signature color = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}});
    Signature mode = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "int"}});
    Signature sameColor = signature("go", {
        {Kind::Enum, "unit::Color"}, {Kind::Primitive, "int"}});
    Signature diffPrimitive = signature("go", {
        {Kind::Enum, "unit::Mode"}, {Kind::Primitive, "string"}});
    Signature arity1 = signature("go", {{Kind::Enum, "unit::Color"}});

    assert(compare(color, mode) == PairStatus::DistinctEnums);
    assert(compare(color, sameColor) == PairStatus::Same);
    assert(compare(color, diffPrimitive) == PairStatus::InvalidSameArity);
    assert(compare(color, arity1) == PairStatus::DistinctArity);
    assert(select({color, mode}, {{0, "unit::Mode"}}) == 1);
    assert(select({color, mode}, {{0, "unit::Color"}}) == 0);

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
    assert(compare(color, imported) == PairStatus::DistinctEnums);

    // This deliberately exercises only compiler-side keys, NOT LLVM source
    // parsing, AOT vtables, actor dispatch or guest execution.
    return 0;
}
