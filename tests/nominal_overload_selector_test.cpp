#include "oreslang/frontend/NominalOverloadSelector.hpp"
#include <cassert>
#include <stdexcept>
#include <vector>

using namespace oreslang;

template <typename F> void must_reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

int main() {
    const auto a = enum_tag("pkg/a.ores::A");
    const auto b = enum_tag("pkg/b.ores::B");
    const auto integer = primitive_tag("int");
    const auto boolean = primitive_tag("bool");

    // Two enums can both hold Foo=1; only declaration identity matters.
    const std::vector<NominalOverloadSlot> cases = {
        {"root", "x", {a}}, {"root", "x", {b}},
        {"root", "x", {integer}}, {"root", "x", {boolean}}
    };
    validate_nominal_overload_family(cases);
    assert(resolve_nominal_overload_static(cases, {a}) == 0);
    assert(resolve_nominal_overload_static(cases, {b}) == 1);
    assert(resolve_nominal_overload_static(cases, {integer}) == 2);
    assert(resolve_nominal_overload_static(cases, {boolean}) == 3);
    assert(nominal_overload_link_key(cases[0]) != nominal_overload_link_key(cases[1]));
    assert(nominal_overload_link_key(cases[2]) != nominal_overload_link_key(cases[3]));

    // Missing tags do not authorize inspecting runtime values.
    must_reject([&] { resolve_nominal_overload_static(cases, {std::nullopt}); });
    must_reject([&] { resolve_nominal_overload_static(cases, {primitive_tag("string")}); });

    // A nominal case member's integer payload is not a distinct type key.
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a}}, {"root", "x", {a}}
        });
    });
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a, std::nullopt}},
            {"root", "x", {a, std::nullopt}}
        });
    });
    // Structural/generic positions cannot prove disjointness.
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a, std::nullopt}},
            {"root", "x", {a, integer}}
        });
    });
    // Two typed positions can prove disjointness even when one is shared.
    validate_nominal_overload_family({
        {"root", "x", {a, integer}},
        {"root", "x", {a, boolean}}
    });
    must_reject([&] { enum_tag("pkg/A#$"); });
    must_reject([&] { primitive_tag("i8"); });
    must_reject([&] {
        validate_nominal_overload_family({
            {"first", "x", {a}}, {"second", "x", {b}}
        });
    });
    return 0;
}
