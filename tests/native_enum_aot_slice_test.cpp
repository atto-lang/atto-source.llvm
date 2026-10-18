// Actual compiler-header test: parses Oreslang source into exact eos2
// function/call identities and rejects invalid calls or truncated exit sums.
// This has no dependencies on LLVM libraries and is suitable for ASan/UBSan.
#include "oreslang/frontend/NativeEnumAotSlice.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <type_traits>

#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "native enum AOT parser failed: %s at %s:%d\n", \
                 #condition, __FILE__, __LINE__); std::abort(); \
} } while (false)

namespace {
void rejects(const std::string& source) {
    bool denied = false;
    try { (void)oreslang::enum_aot::parse(source); }
    catch (const std::invalid_argument&) { denied = true; }
    REQUIRE(denied);
}
std::string fixture(int a = 1, int b = 2, const std::string& first = "10",
                    const std::string& second = "0") {
    return "define enum Foo as A = " + std::to_string(a)
        + ", B = " + std::to_string(b) + " end\n"
        + "fnc classify(Foo.A value): int { return " + first + "; }\n"
        + "fnc classify(Foo value): int { return " + second + "; }\n"
        + "fnc main(): int {\n"
        + " const Foo.A specific = Foo.A;\n"
        + " const Foo widened = Foo.A;\n"
        + " return classify(specific) + classify(widened);\n"
        + "}\n";
}
}
int main() {
    using oreslang::enum_aot::Plan;
    using oreslang::enum_aot::VerifiedPlan;
    static_assert(!std::is_constructible_v<VerifiedPlan, Plan&&>,
                  "an arbitrary mutable plan must not forge a compiler seal");
    static_assert(!std::is_copy_constructible_v<VerifiedPlan>,
                  "the compiler seal cannot be copied");
    static_assert(!std::is_move_constructible_v<VerifiedPlan>,
                  "the compiler seal cannot be moved");
    static_assert(std::is_const_v<std::remove_reference_t<
        decltype(std::declval<const VerifiedPlan&>().readOnly())>>,
        "the sealed source plan must expose only const data");
    const VerifiedPlan sealed = oreslang::enum_aot::parseVerified(fixture());
    REQUIRE(oreslang::enum_aot::checkedNativeExit(sealed.readOnly()) == 10);

    auto plan = oreslang::enum_aot::parse(fixture());
    oreslang::enum_aot::validatePlan(plan);
    auto expectForgedRejected = [&](const oreslang::enum_aot::Plan& tampered) {
        bool rejected = false;
        try { oreslang::enum_aot::validatePlan(tampered); }
        catch (const std::invalid_argument&) { rejected = true; }
        REQUIRE(rejected);
    };
    {
        auto forged = plan;
        forged.returnCalls[0].exactSymbol = forged.returnCalls[1].exactSymbol;
        expectForgedRejected(forged);
        forged = plan;
        forged.returnCalls[0].tag = forged.cases[1].tag;
        expectForgedRejected(forged);
        forged = plan;
        forged.returnCalls[0].staticallyProvenCase = "Missing";
        expectForgedRejected(forged);
        forged = plan;
        forged.enumId = "local_unit.__root__.Other";
        expectForgedRejected(forged);
        forged = plan;
        forged.cases[0].name = "int";
        expectForgedRejected(forged);
        forged = plan;
        forged.cases[0].tag = 0;
        expectForgedRejected(forged);
        forged = plan;
        forged.functions[0].signature.name = "other";
        expectForgedRejected(forged);
    }
    REQUIRE(plan.enumId == "local_unit.__root__.Foo");
    REQUIRE(plan.cases.size() == 2);
    REQUIRE(plan.functions.size() == 2);
    REQUIRE(plan.returnCalls.size() == 2);
    REQUIRE(oreslang::enum_aot::checkedNativeExit(plan) == 10);
    REQUIRE(plan.returnCalls[0].exactSymbol != plan.returnCalls[1].exactSymbol);
    REQUIRE(plan.returnCalls[0].staticallyProvenCase == "A");
    REQUIRE(plan.returnCalls[1].staticallyProvenCase.empty());
    REQUIRE(plan.returnCalls[0].tag == plan.returnCalls[1].tag);

    const auto originalSpecialized = plan.returnCalls[0].exactSymbol;
    const auto originalWhole = plan.returnCalls[1].exactSymbol;
    for (int tag = 1; tag < 1024; tag += 17) {
        const auto newPlan = oreslang::enum_aot::parse(fixture(tag, tag+1));
        REQUIRE(oreslang::enum_aot::checkedNativeExit(newPlan) == 10);
        REQUIRE(newPlan.returnCalls[0].exactSymbol == originalSpecialized);
        REQUIRE(newPlan.returnCalls[1].exactSymbol == originalWhole);
        REQUIRE(newPlan.returnCalls[0].tag == tag);
    }

    rejects(fixture(7, 3));
    rejects(fixture(0, 1));
    rejects(fixture(2147483647, 2147483647));
    rejects(fixture(2147483647, 2147483647, "200", "100"));
    rejects(fixture(1, 2, "200", "100"));
    rejects(fixture(1, 2, "9223372036854775807", "1"));
    rejects(fixture(1, 2, "-9223372036854775807", "-2"));
    rejects(fixture(1, 2, "-5", "0"));

    rejects("define enum Foo as A, A end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc main(): int { return classify(Foo.A); }");
    rejects("define enum Foo as A, B end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc main(): int { const Foo w = Foo.A; return classify(w); }");
    rejects("define enum Foo as A, B end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc classify(Foo.A x): int { return 2; }\n"
            "fnc main(): int { return classify(Foo.A); }");
    rejects("define enum Foo as A, B end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc main(): int { return classify(Foo.Missing); }");
    rejects("define enum Foo as A, B end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc main(): int { const Foo.A a = Foo.B; return classify(a); }");
    rejects("define enum Foo as A, B end\n"
            "fnc classify(Foo.A x): int { return 1; }\n"
            "fnc main(): int { return classify(Foo.A) + classify(Foo.A) + 1; }");

    rejects("define enum enum as A, B end\n"
            "fnc classify(enum.A x): int { return 10; }\n"
            "fnc main(): int { return classify(enum.A); }");
    rejects("define enum Foo as int, B end\n"
            "fnc classify(Foo.B x): int { return 10; }\n"
            "fnc main(): int { return classify(Foo.B); }");
    rejects("define enum Foo.Bar as A, B end\n"
            "fnc classify(Foo.Bar.A x): int { return 10; }\n"
            "fnc main(): int { return classify(Foo.Bar.A); }");
    rejects("define enum Foo as A, B end\n"
            "fnc int(Foo.A x): int { return 10; }\n"
            "fnc main(): int { return int(Foo.A); }");
    // Signed i64 minimum is a legal literal; its unsigned magnitude must
    // parse without an intermediate signed overflow or undefined negation.
    const std::string minValue = std::to_string(std::numeric_limits<std::int64_t>::min());
    const std::string maxValue = std::to_string(std::numeric_limits<std::int64_t>::max());
    const std::string minProgram =
        std::string("define enum Foo as A, B end\n") +
        "fnc classify(Foo.A v): int { return " + minValue + "; }\n" +
        "fnc classify(Foo.B v): int { return " + maxValue + "; }\n" +
        "fnc classify(Foo v): int { return 1; }\n" +
        "fnc main(): int { const Foo widened = Foo.A; "
        "return classify(Foo.A) + classify(Foo.B) + classify(widened); }";
    const auto minPlan = oreslang::enum_aot::parse(minProgram);
    REQUIRE(oreslang::enum_aot::checkedNativeExit(minPlan) == 0);
    rejects(std::string("define enum Foo as A, B end\n") +
        "fnc classify(Foo.A v): int { return " +
        std::to_string(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u)
        + "; }\nfnc main(): int { return classify(Foo.A); }");
    rejects(std::string("define enum Foo as A, B end\n") +
        "fnc classify(Foo.A v): int { return -" +
        std::to_string(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 2u)
        + "; }\nfnc main(): int { return classify(Foo.A); }");
    std::puts("PASS: compiled original native enum AOT frontend policy header");
}
