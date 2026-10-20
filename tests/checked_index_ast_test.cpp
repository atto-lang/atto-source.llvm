#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(1);
}
}

int main() {
    constexpr std::string_view source = R"ores(
fnc read(int value, int key): int {
    return value[key];
}
)ores";

    oreslang::Program program = oreslang::parse_program(source);
    if (program.functions.size() != 1) fail("expected one parsed function");
    const auto& function = program.functions.front();
    if (!function.result) fail("expected parsed return expression");

    const auto& index = *function.result;
    if (index.kind != oreslang::Expression::Kind::Index)
        fail("bracket syntax was not preserved as an Index AST node");
    if (!index.left || index.left->kind != oreslang::Expression::Kind::Variable
            || index.left->name != "value")
        fail("Index receiver identity was not preserved");
    if (!index.right || index.right->kind != oreslang::Expression::Kind::Variable
            || index.right->name != "key")
        fail("Index selector identity was not preserved");
    if (!index.name.empty())
        fail("Index AST must not reuse Field member-name authority");
    return 0;
}
