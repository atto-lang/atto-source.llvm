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
        {"root", "x", {a}}, {"root", "x", {b}}
    };
    validate_nominal_overload_family(cases);
    check(resolve_nominal_overload_static(cases, {a}) == 0);
    check(resolve_nominal_overload_static(cases, {b}) == 1);
    check(nominal_overload_link_key(cases[0]) != nominal_overload_link_key(cases[1]));

    // Missing proof, primitive argument, or unrelated enums never select.
    must_reject([&] { resolve_nominal_overload_static(cases, {std::nullopt}); });
    must_reject([&] { resolve_nominal_overload_static(cases, {integer}); });
    must_reject([&] { resolve_nominal_overload_static(cases, {enum_tag("pkg/c.ores::C")}); });

    // Primitive-only differences and primitive/enum differences are forbidden.
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {integer}}, {"root", "x", {boolean}}
        });
    });
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a}}, {"root", "x", {integer}}
        });
    });
    // Even a DIFFERENT enum cannot excuse an additional primitive difference.
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a, integer}}, {"root", "x", {b, boolean}}
        });
    });
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a}}, {"root", "x", {a}}
        });
    });
    // Unknown/generic signature positions are never eligible discriminators.
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a, std::nullopt}},
            {"root", "x", {b, std::nullopt}}
        });
    });
    // The other, identical scalar position is allowed; it is not a selector.
    const std::vector<NominalOverloadSlot> valid_with_scalar = {
        {"root", "x", {a, integer}}, {"root", "x", {b, integer}}
    };
    validate_nominal_overload_family(valid_with_scalar);
    check(resolve_nominal_overload_static(valid_with_scalar, {b, integer}) == 1);
    // Multiple distinct enum positions are allowed and statically checked.
    validate_nominal_overload_family({
        {"root", "x", {a, a}}, {"root", "x", {b, b}}
    });
    // Java normalizes String to string, rather than using separate ABI slots.
    check(primitive_tag("String") == primitive_tag("string"));
    check(primitive_tag("i8") == NominalTypeTag{NominalTypeTag::Kind::Primitive, "i8"});
    check(primitive_tag("u8") == NominalTypeTag{NominalTypeTag::Kind::Primitive, "u8"});
    must_reject([&] { primitive_tag("float"); });
    must_reject([&] { primitive_tag("decimal"); });
    must_reject([&] { primitive_tag("complex"); });
    must_reject([&] {
        validate_nominal_overload_family({
            {"root", "x", {a, integer}},
            {"root", "x", {b, primitive_tag("string")}}
        });
    });
    must_reject([&] {
        validate_nominal_overload_family(std::vector<NominalOverloadSlot>(
            129, {"root", "x", {a}}));
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
    must_reject([&] { primitive_tag("i128"); });
    // Compiler internal records may bypass constructor factory APIs. All
    // public selectors, including single-member families, validate tags.
    const auto forged_primitive =
        NominalTypeTag{NominalTypeTag::Kind::Primitive, "String"};
    const auto forged_enum =
        NominalTypeTag{NominalTypeTag::Kind::Enum, "A"};
    const auto forged_discriminant =
        NominalTypeTag{static_cast<NominalTypeTag::Kind>(12345), "int"};
    must_reject([&] { validate_nominal_overload_family({{"root", "x", {forged_primitive}}}); });
    must_reject([&] { validate_nominal_overload_family({{"root", "x", {forged_enum}}}); });
    must_reject([&] { validate_nominal_overload_family({{"root", "x", {forged_discriminant}}}); });
    must_reject([&] { resolve_nominal_overload_static({{"root", "x", {integer}}}, {forged_primitive}); });
    must_reject([&] { resolve_nominal_overload_static({{"root", "x", {integer}}}, {forged_discriminant}); });
    must_reject([&] { nominal_overload_link_key({"root", "x", {forged_enum}}); });
    must_reject([&] { nominal_overload_link_key({"root", "x", {forged_discriminant}}); });
    // Aggregate-built signatures must not bypass the 64-position bound.
    // These checks run in Release builds too (no assert-only validation).
    const std::vector<std::optional<NominalTypeTag>> overlong(65, integer);
    must_reject([&] {
        validate_nominal_overload_family({{"root", "x", overlong}});
    });
    must_reject([&] {
        resolve_nominal_overload_static({{"root", "x", {integer}}}, overlong);
    });
    must_reject([&] {
        nominal_overload_link_key({"root", "x", overlong});
    });
    // Also distinguish a truly embedded NUL from a source spelling.
    const std::string embedded_nul("x\\0y", 3);
    check(embedded_nul.size() == 3 && embedded_nul[1] == '\\0');
    check(nominal_overload_link_key({"root", embedded_nul, {a}})
            != nominal_overload_link_key({"root", "x", {a}}));
    must_reject([&] {
        validate_nominal_overload_family({
            {"first", "x", {a}}, {"second", "x", {b}}
        });
    });
    return 0;
}
