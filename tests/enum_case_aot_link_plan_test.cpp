#include "oreslang/types/EnumCaseAotLinkPlan.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "eos2 AOT link: %s at %s:%d\n", \
                 #condition, __FILE__, __LINE__); std::abort(); \
} } while (false)

namespace {
using namespace oreslang::types::case_overloads;
using oreslang::types::case_aot::Image;

void rejects(const std::function<void()>& fn) {
    bool failed = false;
    try { fn(); } catch (const std::invalid_argument&) { failed = true; }
    REQUIRE(failed);
}
Signature sig(std::vector<Parameter> args, Domain domain = Domain::Module) {
    return {domain, "unit.root", "foo", std::move(args)};
}
}

int main() {
    const Registry registry{
        {"unit.Foo", {"unit.Foo", {"A", "B", "C"}, true}},
        {"unit.Bar", {"unit.Bar", {"A", "B"}, true}}
    };
    const Parameter whole = Parameter::whole("unit.Foo");
    const Parameter a = Parameter::selectedCase("unit.Foo", "A");
    const Parameter b = Parameter::selectedCase("unit.Foo", "B");
    const Parameter c = Parameter::selectedCase("unit.Foo", "C");
    const Parameter fixed = Parameter::fixed("int");
    const Signature parent = sig({whole, fixed});
    const Signature specializedA = sig({a, fixed});
    const Signature specializedB = sig({b, fixed});
    std::vector<Signature> family{specializedB, parent, specializedA};
    std::map<std::string, std::string> definitions;
    for (const auto& candidate : family) {
        REQUIRE(definitions.emplace(candidate.symbolId(), std::string(64, 'a')).second);
    }
    Image image(registry, family, definitions);
    REQUIRE(image.definitionCount() == 3);
    auto first = image.bind({a, fixed});
    auto second = image.bind({b, fixed});
    auto widened = image.bind({whole, fixed});
    auto unknown = image.bind({c, fixed});
    REQUIRE(first.symbol() == specializedA.symbolId());
    REQUIRE(second.symbol() == specializedB.symbolId());
    REQUIRE(widened.symbol() == parent.symbolId());
    REQUIRE(unknown.symbol() == widened.symbol());
    REQUIRE(first.symbol() != widened.symbol());
    REQUIRE(first.bodyDigest() == std::string(64, 'a'));
    image.verify(first);
    image.verify(second);
    definitions.clear(); // Image owns its complete immutable definition map.
    family.clear();
    REQUIRE(image.bind({a, fixed}).symbol() == first.symbol());

    rejects([&] { (void)image.bind({Parameter::whole("unit.Bar"), fixed}); });
    rejects([&] { (void)image.bind({Parameter::fixed("float"), fixed}); });
    rejects([&] { (void)image.bind({Parameter::selectedCase("unit.Foo","Missing"), fixed}); });
    rejects([&] { (void)image.bind({whole}); });
    rejects([&] { (void)image.bind({}); });

    std::vector<Signature> original{parent, specializedA, specializedB};
    std::map<std::string, std::string> fullBodies;
    for (const auto& signature : original)
        fullBodies.emplace(signature.symbolId(), std::string(64, 'a'));
    auto missing = fullBodies;
    missing.erase(specializedB.symbolId());
    rejects([&] { Image invalid(registry, original, missing); });
    auto extra = fullBodies;
    extra.emplace("untrusted-eos2", std::string(64, 'f'));
    rejects([&] { Image invalid(registry, original, extra); });
    auto uppercase = fullBodies;
    uppercase[specializedA.symbolId()] = std::string(64, 'A');
    rejects([&] { Image invalid(registry, original, uppercase); });
    auto wronglength = fullBodies;
    wronglength[specializedA.symbolId()] = std::string(63, 'a');
    rejects([&] { Image invalid(registry, original, wronglength); });
    auto virtualCase = sig({a, fixed}, Domain::Instance);
    rejects([&] { Image invalid(registry, {virtualCase},
        {{virtualCase.symbolId(), std::string(64, 'a')}}); });

    Image newer(registry, original, fullBodies);
    rejects([&] { newer.verify(first); });
    std::puts("PASS: native eos2 exact direct AOT link policy (host only)");
}
