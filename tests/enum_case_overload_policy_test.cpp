#include "oreslang/types/EnumCaseOverloadPolicy.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "native enum-case policy: %s failed at %s:%d\n", \
                 #cond, __FILE__, __LINE__); std::abort(); \
} } while (false)

namespace {
using namespace oreslang::types::case_overloads;
void rejects(const std::function<void()>& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);
}
Signature f(std::vector<Parameter> params, Domain d = Domain::Module,
            std::string owner = "root.module") {
    return {d, std::move(owner), "foo", std::move(params)};
}
} // namespace

int main() {
    using namespace oreslang::types::case_overloads;
    const Registry registry{
        {"root.Foo", {"root.Foo", {"A", "B", "C"}, true}},
        {"root.Bar", {"root.Bar", {"A", "B"}, true}},
        {"other.Foo", {"other.Foo", {"A", "B"}, true}},
        {"root.Unsafe", {"root.Unsafe", {"A"}, false}}
    };
    const auto whole = Parameter::whole("root.Foo");
    const auto a = Parameter::selectedCase("root.Foo", "A");
    const auto b = Parameter::selectedCase("root.Foo", "B");
    const auto c = Parameter::selectedCase("root.Foo", "C");
    const auto count = Parameter::fixed("int");
    const Signature parent = f({whole, count});
    const Signature aa = f({a, count});
    const Signature bb = f({b, count});
    const std::vector<Signature> family{parent, aa, bb};
    REQUIRE(aa.symbolId()
        == "eos26_4d4f44554c4511_726f6f742e6d6f64756c653_666f6fa2_C8_726f6f742e466f6f1_41F3_696e74");
    REQUIRE(aa.symbolId() != bb.symbolId());
    REQUIRE(parent.symbolId() != aa.symbolId());
    REQUIRE(f({a,count}, Domain::Static).symbolId() != aa.symbolId());
    REQUIRE(f({a,count}, Domain::Module, "root.other").symbolId() != aa.symbolId());
    REQUIRE(f({Parameter::selectedCase("other.Foo","A"),count}).symbolId() != aa.symbolId());

    REQUIRE(selectStatic(family, registry, {a,count}) == 1);
    REQUIRE(selectStatic(family, registry, {b,count}) == 2);
    REQUIRE(selectStatic(family, registry, {c,count}) == 0);
    REQUIRE(selectStatic(family, registry, {whole,count}) == 0);
    REQUIRE(selectStatic({bb, parent, aa}, registry, {a,count}) == 2);
    REQUIRE(selectStatic({aa,bb}, registry, {b,count}) == 1);
    rejects([&] { (void)selectStatic({aa,bb}, registry, {whole,count}); });
    rejects([&] { (void)selectStatic({aa}, registry, {whole,count}); });
    rejects([&] { (void)selectStatic({aa}, registry, {b,count}); });
    rejects([&] { (void)selectStatic({aa}, registry, {}); });

    const Signature cross1 = f({a, Parameter::whole("root.Bar")});
    const Signature cross2 = f({whole, Parameter::selectedCase("root.Bar", "B")});
    rejects([&] { checkFamily({cross1,cross2}, registry); });
    const Signature nested = f({a, Parameter::selectedCase("root.Bar", "B")});
    checkFamily({f({whole,Parameter::whole("root.Bar")}), cross1, nested}, registry);
    REQUIRE(selectStatic(
        {f({whole,Parameter::whole("root.Bar")}), cross1, nested}, registry,
        {a, Parameter::selectedCase("root.Bar","B")}) == 2);

    checkFamily({f({whole}), f({Parameter::whole("other.Foo")})}, registry);
    rejects([&] { checkFamily({aa,aa}, registry); });
    rejects([&] { checkFamily({f({a, count}),
                              f({b,Parameter::fixed("string")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::fixed("int")}),
                              f({Parameter::fixed("string")})}, registry); });
    rejects([&] { checkFamily({f({whole}),f({count})}, registry); });
    rejects([&] { checkFamily({f({Parameter::selectedCase("root.Foo","Other")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::selectedCase("root.Unsafe","A")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole("forged.Foo")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::fixed("Any")})}, registry); });
    for (const std::string& id : std::vector<std::string>{
            "User", "Foo.A", "i8", "String", "f64",
            "Tuple<int>", "{x:int}", "Unknown", "void"}) {
        rejects([&] { checkFamily({f({Parameter::fixed(id)})}, registry); });
    }
    for (const std::string& id : std::vector<std::string>{
            "int", "bool", "string", "float"}) {
        checkFamily({f({Parameter::fixed(id)})}, registry);
    }
    rejects([&] { checkFamily({f({a}),f({a},Domain::Instance)}, registry); });
    rejects([&] { checkFamily({f({a})}, Registry{{"root.Foo", {"other.Foo", {"A"}, true}}}); });
    rejects([&] { checkFamily({f({a})}, Registry{{"root.Foo", {"root.Foo", {"A","A"}, true}}}); });

    const std::vector<Parameter> oversize(MaxArity+1,count);
    rejects([&] { checkFamily({f(oversize)}, registry); });
    const std::vector<Signature> tooMany(MaxCandidates+1, aa);
    rejects([&] { checkFamily(tooMany, registry); });
    Registry oversizedRegistry;
    for (std::size_t i = 0; i <= MaxEnums; ++i) {
        std::string id = "root.E" + std::to_string(i);
        oversizedRegistry.emplace(id, EnumInfo{id, {"A"}, true});
    }
    rejects([&] { checkFamily({f({count})}, oversizedRegistry); });
    rejects([&] { checkFamily({f({Parameter::whole("root. Foo")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole(
            std::string("root.") + std::string({char(0xe2),char(0x80),char(0xae)}) + "Foo")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::selectedCase("root.Foo",
            std::string("A") + std::string({char(0xe2),char(0x80),char(0x8d)}))})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole("root.\xED\xA0\x80")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole("root.\xC0\x80")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole("root.\xF4\x90\x80\x80")})}, registry); });
    rejects([&] { checkFamily({f({Parameter::whole("root.\x00")})}, registry); });
    rejects([&] { (void)f({a},static_cast<Domain>(99)).symbolId(); });
    rejects([&] { checkFamily({f({Parameter{static_cast<Kind>(99),"root.Foo",""}})}, registry); });
    // Host source-free proof only. No native guest enum syntax is accepted.
    return 0;
}
