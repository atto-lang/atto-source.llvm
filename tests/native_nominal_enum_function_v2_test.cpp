#include "oreslang/frontend/NominalEnumV1.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

static const std::string source = R"atto(
define enum Light as
  Red = 1
  Green = 2
  Yellow = 3
end

fnc next(Light state): Light {
  match state over
    on Light.Red -> { return Light.Green; }
    on Light.Green -> { return Light.Yellow; }
    on Light.Yellow -> { return Light.Red; }
  end
}

pub fnc main(): int {
  val Light active = next(Light.Red);
  match active over
    on Light.Red -> { return 10; }
    on Light.Green -> { return 0; }
    on Light.Yellow -> { return 30; }
  end
}
)atto";

static std::string replace(std::string input,
                           const std::string& original,
                           const std::string& updated) {
    const auto at = input.find(original);
    if (at == std::string::npos)
        throw std::runtime_error("nominal function test mutation anchor absent");
    input.replace(at, original.size(), updated);
    return input;
}
static void reject(std::string input, const std::string& expected) {
    try { (void)oreslang::frontend::lower_nominal_enum_v1(input); }
    catch (const std::runtime_error& failure) {
        if (std::string(failure.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error("wrong nominal function diagnostic: " +
            std::string(failure.what()) + " expected " + expected);
    }
    throw std::runtime_error("nominal function accepted invalid source: " + expected);
}
int main() {
    try {
        const auto lowered = oreslang::frontend::lower_nominal_enum_v1(source);
        if (lowered.find("fnc next(int state): int") == std::string::npos ||
                lowered.find("if state == 1 then return 2; fi") == std::string::npos ||
                lowered.find("if state == 2 then return 3; fi") == std::string::npos ||
                lowered.find("return 1;") == std::string::npos ||
                lowered.find("val int active = next(1);") == std::string::npos ||
                lowered.find("if active == 2 then return 0; fi") == std::string::npos ||
                lowered.find("define enum Light") != std::string::npos)
            throw std::runtime_error("typed enum helper was not lowered to real native calls");
        // Preserve actual call semantics across all three input cases.
        for (const auto& variant : {"Light.Green", "Light.Yellow"}) {
            auto changed = replace(source, "next(Light.Red)",
                                   std::string("next(") + variant + ")");
            if (oreslang::frontend::lower_nominal_enum_v1(changed).find(
                    std::string("active = next(") +
                    (std::string(variant) == "Light.Green" ? "2" : "3") + ");") ==
                std::string::npos)
                throw std::runtime_error("nominal helper call changed variant identity");
        }

        reject(replace(source, "next(Light state)", "next(Other state)"),
            "expected 'Light'");
        reject(replace(source, "): Light {", "): Other {"),
            "expected 'Light'");
        reject(replace(source, "on Light.Red -> { return Light.Green; }",
            "on Other.Red -> { return Light.Green; }"),
            "helper pattern foreign nominal owner");
        reject(replace(source, "on Light.Yellow -> { return Light.Red; }",
            "on Light.Green -> { return Light.Red; }"),
            "helper duplicate match arm");
        reject(replace(source, "on Light.Yellow -> { return Light.Red; }", ""),
            "helper nonexhaustive nominal match");
        reject(replace(source, "return Light.Yellow;", "return Other.Yellow;"),
            "helper return foreign nominal owner");
        reject(replace(source, "return Light.Yellow;", "return Light.Blue;"),
            "helper returns unknown nominal case");
        reject(replace(source, "return Light.Yellow;", "return 3;"),
            "unreserved identifier");
        reject(replace(source, "next(Light.Red)", "next(1)"),
            "unreserved identifier");
        reject(replace(source, "next(Light.Red)", "next(Other.Red)"),
            "foreign nominal owner");
        reject(replace(source, "next(Light.Red)", "next()"),
            "unreserved identifier");
        reject(replace(source, "match state over", "match other over"),
            "expected 'state'");
        reject(replace(source, "pub fnc main",
            "fnc extra(Light v): Light { } pub fnc main"), "expected 'pub'");
        reject(replace(source, "fnc next(", "pub fnc next("),
            "expected 'main'");
        reject(replace(source, "return Light.Yellow;", "return Light.Red(2);"),
            "expected ')'");
        reject(replace(source, "on Light.Red -> { return 10; }",
            "on Other.Red -> { return 10; }"),
            "match has foreign nominal owner");

        std::string commented = "/* fnc evil(Other state): Other {} */\n";
        commented += source;
        commented += "\n// fnc next(int state): int { return 100; }\n";
        if (oreslang::frontend::lower_nominal_enum_v1(commented) != lowered)
            throw std::runtime_error("comment trivia altered verified nominal helper");
        std::cout << "ATTO_LLVM_NOMINAL_FUNCTION_V2_HOST_PASS=16\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "ATTO_LLVM_NOMINAL_FUNCTION_V2_HOST_FAIL="
                  << failure.what() << '\n';
        return 1;
    }
}
