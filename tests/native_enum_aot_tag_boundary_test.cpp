#include "oreslang/frontend/NativeEnumAotSlice.hpp"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <cstdio>

static bool denied(const std::string& s) {
    try { (void)oreslang::enum_aot::parseVerified(s); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}
int main() {
    const std::string good =
        "define enum Foo as A = 2147483646, B = 2147483647 end\n"
        "fnc classify(Foo.A v): int { return 100; }\n"
        "fnc classify(Foo v): int { return 1; }\n"
        "fnc main(): int { const Foo widened = Foo.A; "
        "return classify(widened); }";
    const auto p = oreslang::enum_aot::parseVerified(good);
    if (p.readOnly().cases.at(1).tag != std::numeric_limits<std::int32_t>::max())
        return 1;
    if (!p.readOnly().returnCalls.at(0).staticallyProvenCase.empty())
        return 2;
    if (oreslang::enum_aot::checkedNativeExit(p.readOnly()) != 1)
        return 3;
    const std::string invalid =
        "define enum Foo as A = 2147483647, B end\n"
        "fnc classify(Foo.A v): int { return 100; }\n"
        "fnc classify(Foo v): int { return 1; }\n"
        "fnc main(): int { return classify(Foo.A); }";
    if (!denied(invalid)) return 4;
    std::puts("PASS: native enum exact max-tag and nominal widening");
}
