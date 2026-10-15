// Source-level native frontend smoke; compile Parser.cpp/OwnershipChecker.cpp
// without LLVM codegen, then verify its JSON with common-source IR v1.
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ExperimentalHirV1.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

static constexpr std::string_view SOURCE = R"ores(
pub fnc counter_response(int count): int {
  return count * 43;
}
pub fnc main(): int {
  return counter_response(1);
}
)ores";

int main() {
    const auto first = oreslang::parse_program(SOURCE);
    const std::string json = oreslang::emit_experimental_hir_v1(first);
    const auto second = oreslang::parse_program(SOURCE);
    if (json != oreslang::emit_experimental_hir_v1(second))
        throw std::runtime_error("nondeterministic native source-to-HIR serialization");
    assert(json.find("\"function\":\"counter_response\"") != std::string::npos);
    assert(json.find("\"value\":\"43\"") != std::string::npos);

    // Unknown arithmetic semantics must never silently lower to accepted HIR.
    bool rejected = false;
    try {
        const auto unsupported = oreslang::parse_program(
            "pub fnc main(): int { return 8 / 2; }");
        (void) oreslang::emit_experimental_hir_v1(unsupported);
    } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("division must fail closed");

    rejected = false;
    try {
        const auto unsupported = oreslang::parse_program(
            "pub fnc main(): int { let int v = 3; return v; }");
        (void) oreslang::emit_experimental_hir_v1(unsupported);
    } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("local statements must fail closed");

    std::cout << json;
}
