#include "oreslang/frontend/Parser.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void expect_rejected(std::string_view source, std::string_view expected) {
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::runtime_error& failure) {
        if (std::string_view(failure.what()).find(expected) != std::string_view::npos)
            return;
        throw std::runtime_error(std::string("incorrect import rejection: ")
            + failure.what());
    }
    throw std::runtime_error("unadmitted import parsed successfully");
}
}

int main() {
    try {
        auto parsed = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return 42; }");
        if (parsed.imports.size() != 1
                || parsed.imports[0].symbol != "Assert"
                || parsed.imports[0].canonical_path != "std/assert"
                || parsed.functions.size() != 1) {
            throw std::runtime_error("canonical std/assert import metadata missing");
        }
        expect_rejected(
            "define module Entry as "
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return 42; } end",
            "native module imports must precede declarations");

        auto prelude = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "define module Entry as "
            "pub fnc main(): int { return 42; } end");
        if (prelude.module != "Entry" || prelude.imports.size() != 1)
            throw std::runtime_error("file-level import prelude metadata missing");

        expect_rejected(
            "import module Evil from \"std/assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"./stdlib/assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"../std/assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"std\\/assert\"; "
            "pub fnc main(): int { return 0; }",
            "unsupported escape/control");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return 0; }",
            "duplicate native module imports");
        expect_rejected(
            "import module Assert from \"std/assert; "
            "pub fnc main(): int { return 0; }",
            "unterminated native string");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return 0; } "
            "import module Assert from \"std/assert\";",
            "native module imports must precede declarations");
        std::cout << "NATIVE_ASSERT_IMPORT_PARSER_CONTRACT_PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
