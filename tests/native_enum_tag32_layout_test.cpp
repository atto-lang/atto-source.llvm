#include "oreslang/types/EnumTag32Layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "enum tag32 layout failed: %s at %s:%d\n", \
                 #expr, __FILE__, __LINE__); std::abort(); \
} } while (false)

namespace {
void rejected(const std::function<void()>& run) {
    bool caught = false;
    try { run(); } catch (const std::invalid_argument&) { caught = true; }
    REQUIRE(caught);
}
std::string hex(const std::string& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[(c >> 4) & 0xf];
        out += digits[c & 0xf];
    }
    return out;
}
}

int main() {
    using namespace oreslang::types::enum_tags;
    const std::vector<CaseTag> cases{{"A",5},{"B",17},{"C",31}};
    const EnumTag32Layout p("local_unit.__root__.Foo", cases);
    REQUIRE(p.caseCount() == 3);
    REQUIRE(p.constantTag("A") == 5);
    REQUIRE(p.constantTag("B") == 17);
    REQUIRE(p.constantTag("C") == 31);
    REQUIRE(p.containsWireTag(17));
    REQUIRE(!p.containsWireTag(16));
    REQUIRE(!p.containsWireTag(-1));
    REQUIRE(!p.exported());
    const EnumTag32Layout pub("local_unit.__root__.Foo", cases, true);
    REQUIRE(pub.exported());
    REQUIRE(pub.canonicalSchemaBytes() != p.canonicalSchemaBytes());
    // Public export changes the linked schema while retaining case tags:
    REQUIRE(pub.constantTag("B") == p.constantTag("B"));
    REQUIRE(hex(pub.canonicalSchemaBytes()) ==
        "000000216f7265732d6e756c6c6172792d656e756d2d74616733322d736368656d612d7632"
        "000000176c6f63616c5f756e69742e5f5f726f6f745f5f2e466f6f"
        "000000067075626c6963"
        "0000000133"
        "000000014100000005"
        "000000014200000011"
        "00000001430000001f");
    // Java EnumTag32Plan hashes exactly these length-prefixed bytes using SHA-256.
    REQUIRE(hex(p.canonicalSchemaBytes()) ==
        "000000216f7265732d6e756c6c6172792d656e756d2d74616733322d736368656d612d7632"
        "000000176c6f63616c5f756e69742e5f5f726f6f745f5f2e466f6f"
        "0000000770726976617465"
        "0000000133"
        "000000014100000005"
        "000000014200000011"
        "00000001430000001f");
    rejected([&] { (void)p.constantTag("Missing"); });
    rejected([&] { (void)EnumTag32Layout("unqualified", cases); });
    rejected([&] { (void)EnumTag32Layout("unit." + std::string(508, 'A'), cases); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"A",2},{"B",1}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"A",1},{"A",2}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"Bad.Name",1}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"A",0}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"A",-1}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo", {{"A",1},{"B",1}}); });
    rejected([&] { (void)EnumTag32Layout("unit.Foo",
        std::vector<CaseTag>(MaxCases+1,{"A",1})); });
    const EnumTag32Layout changed("local_unit.__root__.Foo", {{"A",5},{"B",18},{"C",31}});
    REQUIRE(changed.canonicalSchemaBytes() != p.canonicalSchemaBytes());
    const EnumTag32Layout renamed("local_unit.__root__.Bar", cases);
    REQUIRE(renamed.canonicalSchemaBytes() != p.canonicalSchemaBytes());
    std::puts("PASS: native nullary tag32 AOT host layout");
}
