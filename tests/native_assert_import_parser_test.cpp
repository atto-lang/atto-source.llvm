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
        // No attacker-controlled aliases, path variants, or control bytes
        // may be mistaken for the sealed, source-owned standard library.
        expect_rejected(
            "import module Assert from \"std/assert/\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"std//assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"STD/assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"std/assert/../assert\"; "
            "pub fnc main(): int { return 0; }",
            "only sealed Assert");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "define module Entry as "
            "pub fnc main(): int { return 0; } "
            "import module Assert from \"std/assert\"; end",
            "native module imports must precede declarations");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "import module Assert from \"std/assert\"; "
            "define module Entry as pub fnc main(): int { return 0; } end",
            "duplicate native module imports");
        // Source-bound qualified calls are represented as module calls,
        // never as a scalar class field or an untyped host symbol. A bool
        // signature is retained only behind the import/codegen admission gate.
        auto typed = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert.equal(7, 7); } "
            "pub fnc main(): int { return 0; }");
        if (typed.functions.size() != 2 ||
            !typed.functions[0].returns_bool ||
            !typed.functions[0].result ||
            typed.functions[0].result->kind != oreslang::Expression::Kind::ModuleCall ||
            typed.functions[0].result->name != "equal" ||
            !typed.functions[0].result->left ||
            typed.functions[0].result->left->name != "Assert" ||
            typed.functions[0].result->arguments.size() != 2)
            throw std::runtime_error("typed Assert module-call AST missing");

        // Both call delimiters bind to the exact same sealed module AST.
        // Ordinary throw + call-site trap are syntax/model admission only
        // until the native Option / failure unwinder exists.
        auto both = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "pub fnc equal_colon(): bool { return Assert::equal(7, 7); } "
            "pub fnc main(): int { "
            "  const success = Assert.equal_throws(10, 10); "
            "  const failure = trap Assert::equal_throws(10, 20); "
            "  return 0; }");
        if (both.functions.size() != 2 ||
                !both.functions[0].result ||
                both.functions[0].result->kind != oreslang::Expression::Kind::ModuleCall ||
                both.functions[0].result->name != "equal" ||
                both.functions[1].locals.size() != 2 ||
                both.functions[1].locals[0].initializer->kind !=
                    oreslang::Expression::Kind::ModuleCall ||
                both.functions[1].locals[1].initializer->kind !=
                    oreslang::Expression::Kind::Trap ||
                !both.functions[1].locals[1].initializer->left ||
                both.functions[1].locals[1].initializer->left->kind !=
                    oreslang::Expression::Kind::ModuleCall)
            throw std::runtime_error("scoped Assert :: / call-site trap AST did not preserve effects");
        auto reversed = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { "
            "  const success = Assert::equal_throws(10, 10); "
            "  const failure = trap Assert.equal_throws(10, 20); "
            "  return 0; }");
        if (reversed.functions[0].locals.size() != 2 ||
                reversed.functions[0].locals[1].initializer->kind !=
                    oreslang::Expression::Kind::Trap)
            throw std::runtime_error("mixed qualified assertion spellings are not equivalent");

        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert::unknown(1, 1); }",
            "unknown sealed Assert member");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert:::equal(1, 1); }",
            "expected sealed Assert member");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Worker::equal(1, 1); }",
            "requires a sealed static/module namespace");
        expect_rejected(
            "pub fnc main(): int { return Assert.equal(1, 1); }",
            "unbound native Assert module alias");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert.eqaul(1, 1); }",
            "unknown sealed Assert member");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert.equal(1); }",
            "wrong sealed Assert member arity");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert.equal; }",
            "native Assert member must be called");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(int Assert): int { return 0; }",
            "sealed Assert module alias cannot be shadowed");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc Assert(): int { return 0; }",
            "sealed Assert module alias cannot be shadowed");
        expect_rejected(
            "pub fnc check(): bool { return 1; }",
            "native bool return requires sealed module ABI linking");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(int x): bool { return Assert.equal(rt borrow x, 7); }",
            "borrowed reference may only be passed to a borrowed parameter");
        std::cout << "NATIVE_ASSERT_IMPORT_PARSER_CONTRACT_PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
