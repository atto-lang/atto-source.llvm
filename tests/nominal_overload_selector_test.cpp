#include "oreslang/frontend/NominalOverloadSelector.hpp"
#include <stdexcept>
#include <vector>

using namespace oreslang;

static void check(bool condition) {
    if (!condition) throw std::runtime_error("nominal selector regression failed");
}

template <typename F> void must_reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
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
    check(resolve_nominal_overload_static(cases, {a}) == 0);
    check(resolve_nominal_overload_static(cases, {b}) == 1);
    check(resolve_nominal_overload_static(cases, {integer}) == 2);
    check(resolve_nominal_overload_static(cases, {boolean}) == 3);
    check(nominal_overload_link_key(cases[0]) != nominal_overload_link_key(cases[1]));
    check(nominal_overload_link_key(cases[2]) != nominal_overload_link_key(cases[3]));

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
    // Escaping is mandatory: these two pairs collided under
    // owner + "::" + name concatenation.
    check(nominal_overload_link_key({"pkg::module", "x", {a}})
            != nominal_overload_link_key({"pkg", "module::x", {a}}));
    check(nominal_overload_link_key({"root", "x$arity1$e1_a", {a}})
            != nominal_overload_link_key({"root", "x", {a}}));
    check(nominal_overload_link_key({"a", "bc", {a}})
            != nominal_overload_link_key({"ab", "c", {a}}));
    check(nominal_overload_link_key({"root", std::string("x\\0y", 3), {a}})
            != nominal_overload_link_key({"root", "x", {a}}));
    must_reject([&] { nominal_overload_link_key({"", "x", {a}}); });
    must_reject([&] { nominal_overload_link_key({"root", "", {a}}); });
    must_reject([&] { enum_tag("A"); }); // unqualified enums cannot cross files
    must_reject([&] { enum_tag("pkg/a.ores::0A"); });
    must_reject([&] { enum_tag("pkg/a.ores::A-B"); });
    must_reject([&] { enum_tag("pkg/A#$"); });
    must_reject([&] { primitive_tag("i8"); });
    must_reject([&] {
        validate_nominal_overload_family({
            {"first", "x", {a}}, {"second", "x", {b}}
        });
    });
    return 0;
}
