#include "oreslang/frontend/NominalEnumV1.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static const std::string input = R"atto(
define enum Light as
  Red = 1
  Green = 2
  Yellow = 3
end

pub fnc main(): int {
  val Light active = Light.Green;
  match active over
    on Light.Red -> { return 10; }
    on Light.Green -> { return 20; }
    on Light.Yellow -> { return 30; }
  end
}
)atto";
static std::string replace(std::string src,
                           const std::string& from, const std::string& to) {
    const auto offset = src.find(from);
    if (offset == std::string::npos)
        throw std::runtime_error("nominal test fixture drift: " + from);
    src.replace(offset, from.size(), to);
    return src;
}
static void reject(const std::string& source,
                   const std::string& diagnostic) {
    try {
        (void)oreslang::frontend::lower_nominal_enum_v1(source);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos) return;
        throw std::runtime_error("wrong nominal rejection: " +
            std::string(e.what()) + " expected " + diagnostic);
    }
    throw std::runtime_error("unsafe nominal source was admitted: " + diagnostic);
}
int main() {
    try {
        const auto output = oreslang::frontend::lower_nominal_enum_v1(input);
        if (output.find("val int active = 2;") == std::string::npos ||
                output.find("if active == 1 then return 10; fi") == std::string::npos ||
                output.find("if active == 2 then return 20; fi") == std::string::npos ||
                output.find("return 30;") == std::string::npos ||
                output.find("define enum") != std::string::npos)
            throw std::runtime_error("checked symbols were not lowered to native CFG source");

        // No nominal-tag numeric ABI is exposed to source or caller, and
        // every invalid kind fails before original LLVM source lowering.
        reject(replace(input, "Light.Green;", "Other.Green;"), "foreign nominal owner");
        reject(replace(input, "Light.Green;", "Light.Blue;"), "unknown nominal case");
        reject(replace(input, "on Light.Red", "on Other.Red"), "foreign nominal owner");
        reject(replace(input, "on Light.Red", "on Light.Green"), "duplicate match arm");
        reject(replace(input, "on Light.Yellow -> { return 30; }", ""),
            "nonexhaustive");
        reject(replace(input, "Yellow = 3", "Green = 3"), "duplicate nominal case");
        reject(replace(input, "Yellow = 3", "Yellow = 2"), "duplicate/invalid");
        reject(replace(input, "Red = 1", "Red = 0"), "duplicate/invalid");
        reject(replace(input, "Red = 1", "Red = 2147483648"), "out of signed i32");
        reject(replace(input, "Light.Green;", "2;"), "identifier");
        reject(replace(input, "Green = 2", "Green: int = 2"), "identifier");
        reject(replace(input, "on Light.Red ->", "on Light.Red when true ->"),
            "expected '->'");
        reject(replace(input, "main(): int", "main(Light x): int"), "expected ')'");
        reject(replace(input, "val Light active", "let Light active"),
            "expected 'val'");
        reject(input + "define enum Other as New = 1 end", "expected ''");

        std::string trivia = "/* define enum Fake as Ghost = 8 end */\n";
        trivia += input;
        trivia += "\n// on Fake.Ghost -> { return 90; }\n";
        if (oreslang::frontend::lower_nominal_enum_v1(trivia) != output)
            throw std::runtime_error("nominal tokens in comment entered translation");

        std::cout << "ATTO_LLVM_NOMINAL_ENUM_V1_HOST_PASS=15\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ATTO_LLVM_NOMINAL_ENUM_V1_HOST_FAIL: " << e.what() << '\n';
        return 1;
    }
}
