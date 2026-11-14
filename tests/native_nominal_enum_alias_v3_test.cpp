#include "oreslang/frontend/NominalEnumV1.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

// Real opt-in LLVM compiler frontend header. Test emits scalar source that
// the existing oreslang-llvmc lowers to LLVM IR. No hand-written enum tags
// are ever accepted in the .atto guest source.
static const std::string source = R"atto(
define enum Light {
  Red,
  Green,
  Yellow
}
fnc next(Light state): Light {
  do match state over
    on Red as original -> { return original; }
    on Green as g -> { return Light.Yellow; }
    on Yellow -> { return Light.Red; }
  end
}
pub fnc main(): int {
  val Light active = next(Light.Red);
  do match active over
    on Red as r -> { return 11; }
    on Green as g -> { return 22; }
    on Yellow -> { return 33; }
  end
}
)atto";

static std::string changed(std::string input, const std::string& a, const std::string& b) {
    const auto index = input.find(a);
    if (index == std::string::npos)
        throw std::runtime_error("mutation anchor missing: " + a);
    input.replace(index, a.size(), b);
    return input;
}
static void expect_rejected(const std::string& text, const std::string& diagnostic) {
    try { (void)oreslang::frontend::lower_nominal_enum_v1(text); }
    catch (const std::runtime_error& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error("wrong diagnostic: expected " + diagnostic
            + ", got " + e.what());
    }
    throw std::runtime_error("invalid .atto was accepted: " + diagnostic);
}
int main() {
    try {
        const std::string lower = oreslang::frontend::lower_nominal_enum_v1(source);
        if (lower.find("fnc next(int state): int") == std::string::npos
                || lower.find("if state == 1 then return 1; fi") == std::string::npos
                || lower.find("if state == 2 then return 3; fi") == std::string::npos
                || lower.find("val int active = next(1);") == std::string::npos
                || lower.find("if active == 1 then return 11; fi") == std::string::npos
                || lower.find("if active == 2 then return 22; fi") == std::string::npos
                || lower.find("return 33;") == std::string::npos
                || lower.find("define enum Light") != std::string::npos)
            throw std::runtime_error("expected statically narrowed alias dispatch in scalar lowering");

        // No contextual alias can shadow a nominal enum, a variant, or the
        // matched parameter. Malformed patterns fail before emitting IR.
        expect_rejected(changed(source, "on Red as original",
            "on Other.Red as original"), "helper pattern foreign nominal owner");
        expect_rejected(changed(source, "on Red as original",
            "on Blue as original"), "helper pattern unknown case");
        expect_rejected(changed(source, "on Red as original",
            "on Red as state"), "helper alias shadows");
        expect_rejected(changed(source, "on Red as original",
            "on Red as Red"), "alias shadows nominal declaration or variant");
        expect_rejected(changed(source, "on Red as original",
            "on Red as Light"), "alias shadows nominal declaration or variant");
        expect_rejected(changed(source, "return original;",
            "return unknown;"), "expected '.'");
        expect_rejected(changed(source, "on Green as g ->",
            "on Red as g ->"), "helper duplicate match arm");
        expect_rejected(changed(source, "    on Yellow -> { return Light.Red; }\n",
            ""), "helper nonexhaustive nominal match");
        expect_rejected(changed(source, "on Red as r ->",
            "on Other.Red as r ->"), "match has foreign nominal owner");
        expect_rejected(changed(source, "on Red as r ->",
            "on Red as active ->"), "match alias shadows");
        expect_rejected(changed(source, "on Red as r ->",
            "on Blue as r ->"), "match has unknown case");
        expect_rejected(changed(source, "Red,\n  Green,",
            "Red\n  Green,"), "expected comma");
        expect_rejected(changed(source, "Red,\n  Green,",
            "Red,\n  Red,"), "duplicate nominal case");
        expect_rejected(changed(source, "on Red as original",
            "on Red { job_id } as original"), "named enum payload patterns require typed payload AOT lowering");
        expect_rejected(changed(source, "Red,\n  Green,",
            "Red { job_id: int },\n  Green,"),
            "payload-bearing enums require checked payload AOT lowering");
        std::cout << "ATTO_LLVM_NOMINAL_ALIAS_V3_HOST_PASS=15\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ATTO_LLVM_NOMINAL_ALIAS_V3_HOST_FAIL=" << e.what() << '\n';
        return 1;
    }
}
