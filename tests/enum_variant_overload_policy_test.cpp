#include "oreslang/types/EnumVariantOverloadPolicy.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#define REQUIRE(expr) do { if (!(expr)) { \
  std::fprintf(stderr, "native enum variant policy failed: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
  std::abort(); } } while (false)
using namespace oreslang::types::variant_overloads;
namespace {
void rejects(const std::function<void()>& fn) {
    bool did = false;
    try { fn(); } catch (const std::invalid_argument&) { did = true; }
    REQUIRE(did);
}
Signature sig(std::vector<Parameter> p) { return {"file::module", Domain::Module, "f", std::move(p)}; }
}
int main() {
    const Registry registry = {{"file::Foo", {"file::Foo", {{"A", Payload::Nullary},
        {"B", Payload::Nullary}, {"C", Payload::Primitive}}}},
        {"file::Bar", {"file::Bar", {{"C", Payload::Nullary}, {"D", Payload::PrimitiveTuple}}}}};
    const Parameter foo{Kind::Enum, "file::Foo", ""};
    const Parameter a{Kind::Variant, "file::Foo", "A"};
    const Parameter b{Kind::Variant, "file::Foo", "B"};
    const Parameter c{Kind::Variant, "file::Foo", "C"};
    const Parameter bar{Kind::Enum, "file::Bar", ""};
    const Parameter n{Kind::Primitive, "int", ""};
    const auto general = sig({foo, n});
    const auto aSpecific = sig({a, n});
    const auto bSpecific = sig({b, n});
    const auto other = sig({bar, n});
    const std::vector<Signature> family{general, aSpecific, bSpecific, other};
    admit(registry, family);
    REQUIRE(select(registry, family, {a, n}) == 1);
    REQUIRE(select(registry, family, {b, n}) == 2);
    REQUIRE(select(registry, family, {c, n}) == 0);
    REQUIRE(select(registry, family, {foo, n}) == 0);
    REQUIRE(select(registry, family, {bar, n}) == 3);
    rejects([&] { (void)select(registry, {aSpecific, bSpecific}, {foo, n}); });
    rejects([&] { admit(registry, {sig({a, foo}), sig({foo, b})}); });
    rejects([&] { admit(registry, {sig({a, n}), sig({b, {Kind::Primitive, "bool", ""}})}); });
    rejects([&] { admit(registry, {general, general}); });
    rejects([&] { admit(registry, {sig({n}), sig({{Kind::Primitive, "bool", ""}})}); });
    rejects([&] { (void)select(registry, family, {{Kind::Variant, "file::Foo", "Missing"}, n}); });
    rejects([&] { admit(registry, {sig({{Kind::Enum, "file::Unknown", ""}})}); });
    Registry extraComplex = registry;
    extraComplex.emplace("file::Unrelated", EnumInfo{"file::Unrelated", {{"Unsafe", Payload::Complex}}});
    REQUIRE(select(extraComplex, family, {a, n}) == 1);
    Registry unknownLayout = registry;
    unknownLayout.at("file::Foo").cases.at("C") = static_cast<Payload>(99);
    rejects([&] { admit(unknownLayout, family); });
    Signature forgedDomain = general;
    forgedDomain.domain = static_cast<Domain>(99);
    rejects([&] { admit(registry, {forgedDomain}); });
    Registry changed = registry;
    changed.at("file::Foo").cases.at("C") = Payload::Complex;
    rejects([&] { admit(changed, family); });
    Registry tooMany = registry;
    for (std::size_t i = 0; i <= MaxCases; ++i) {
        tooMany.at("file::Foo").cases["Case" + std::to_string(i)] = Payload::Nullary;
    }
    rejects([&] { admit(tooMany, family); });
    rejects([&] { admit(registry, std::vector<Signature>(MaxSameArity + 1, general)); });
    rejects([&] { admit(registry, std::vector<Signature>(MaxFamily + 1, general)); });
    REQUIRE(symbol(aSpecific) != symbol(bSpecific));
    REQUIRE(symbol(general) != symbol(aSpecific));
    REQUIRE(symbol(sig({a})) != symbol(sig({foo})));
    REQUIRE(symbol(aSpecific) == "ores-enum-variant-slot-v1|12:66696c653a3a6d6f64756c65|6:4d4f44554c45|1:66|1:32|1:56|9:66696c653a3a466f6f|1:41|1:50|3:696e74");

    // AOT linker oracle: all definitions exist, exact checked target symbol
    // is frozen, and an image from another compilation cannot accept it.
    std::map<std::string, std::string> bodies;
    for (const auto& entry : family) bodies.emplace(validatedSymbol(registry, entry),
                                                    std::string(64, 'a'));
    AotImage image(registry, family, bodies);
    REQUIRE(image.definitionCount() == family.size());
    const DirectRelocation rA = image.bindDirect({a, n});
    REQUIRE(rA.targetSymbol == validatedSymbol(registry, aSpecific));
    REQUIRE(rA.exactBodyDigest == std::string(64, 'a'));
    image.requireImage(rA);
    REQUIRE(image.bindDirect({bar, n}).targetSymbol == validatedSymbol(registry, other));
    REQUIRE(image.bindDirect({foo, n}).targetSymbol == validatedSymbol(registry, general));
    rejects([&] { (void)image.bindDirect({{Kind::Variant, "file::Foo", "Z"}, n}); });
    rejects([&] { (void)image.bindDirect({{Kind::Primitive, "string", ""}, n}); });
    auto missingBodies = bodies;
    missingBodies.erase(validatedSymbol(registry, aSpecific));
    rejects([&] { AotImage invalid(registry, family, missingBodies); });
    auto unexpectedBodies = bodies;
    unexpectedBodies["untrusted::fake"] = std::string(64, 'b');
    rejects([&] { AotImage invalid(registry, family, unexpectedBodies); });
    auto upperDigest = bodies;
    upperDigest[validatedSymbol(registry, aSpecific)] = std::string(64, 'A');
    rejects([&] { AotImage invalid(registry, family, upperDigest); });
    AotImage anotherImage(registry, family, bodies);
    rejects([&] { anotherImage.requireImage(rA); });
    auto swappedDigest = rA;
    swappedDigest.exactBodyDigest = std::string(64, 'f');
    rejects([&] { image.requireImage(swappedDigest); });
    bodies.clear(); // The compilation image owns its immutable exports.
    REQUIRE(image.bindDirect({a, n}).targetSymbol == rA.targetSymbol);
    // Closed-world native compiler policy only; actual guest enum dispatch remains disabled.
    std::puts("PASS native enum variant host policy");
}
