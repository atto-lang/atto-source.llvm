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
            "native bool result requires typed bool expression");
        expect_rejected(
            "pub fnc main(): int { return true; }",
            "native int return cannot return bool");
        expect_rejected(
            "pub fnc main(): bool { return true; }",
            "native bool main violates process exit ABI");
        expect_rejected(
            "pub fnc choose(int x): bool { return true; } "
            "pub fnc main(): int { return 0; }",
            "native bool literal AOT requires pure zero-argument function");
        expect_rejected(
            "pub fnc main(): int { return true + 1; }",
            "typed bool cannot be used as native int");
        expect_rejected(
            "pub fnc choice(): bool { return true; } "
            "pub fnc main(): int { return choice(); }",
            "typed bool result cannot be used as an int expression");
        auto literal_bool = oreslang::parse_program(
            "pub fnc yes(): bool { return true; } "
            "pub fnc no(): bool { return false; } "
            "pub fnc main(): int { return 0; }");
        if (literal_bool.functions.size() != 3 ||
            !literal_bool.functions[0].returns_bool ||
            !literal_bool.functions[1].returns_bool ||
            literal_bool.functions[0].result->kind !=
                oreslang::Expression::Kind::Boolean ||
            literal_bool.functions[0].result->integer != 1 ||
            literal_bool.functions[1].result->integer != 0)
            throw std::runtime_error("native typed bool literal AST mismatch");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(int x): bool { return Assert.equal(rt borrow x, 7); }",
            "borrowed reference may only be passed to a borrowed parameter");

        // Both spellings share the sealed import binding and compile-time
        // arity rules; neither bypasses native codegen's import rejection.
        auto colon = oreslang::parse_program(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert::equal(7, 7); } "
            "pub fnc main(): int { return 0; }");
        if (colon.functions.size() != 2 || !colon.functions[0].result ||
            colon.functions[0].result->kind != oreslang::Expression::Kind::ModuleCall ||
            colon.functions[0].result->name != "equal" ||
            colon.functions[0].result->arguments.size() != 2)
            throw std::runtime_error("qualified :: Assert call AST mismatch");
        expect_rejected(
            "pub fnc main(): int { return Assert::equal(1, 1); }",
            "unbound native Assert module alias");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert::equal_throws(1); }",
            "wrong sealed Assert member arity");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { return Assert::unknown(1); }",
            "unknown sealed Assert member");
        // Imported bool/String parameters are not yet representable by the
        // native scalar AST. They must not be silently coerced from ints.
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert::true(1); } "
            "pub fnc main(): int { return 0; }",
            "requires typed bool/String argument lowering");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert.false_throws(0); } "
            "pub fnc main(): int { return 0; }",
            "requires typed bool/String argument lowering");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc main(): int { Assert::fail(1); return 0; }",
            "requires typed bool/String argument lowering");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert::false(0); } "
            "pub fnc main(): int { return 0; }",
            "requires typed bool/String argument lowering");


        // A sealed module name must not be hidden by actor state, implicit
        // handler arguments or catch-local exceptional control bindings.
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "define actor Worker as "
            "let Assert: int = 0; "
            "pub fnc on_message(int input): int { return input; } end "
            "pub fnc main(): int { return 0; }",
            "sealed Assert module alias cannot be shadowed");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "define actor Worker as "
            "let count: int = 0; "
            "pub fnc on_message(int Assert): int { return Assert; } end "
            "pub fnc main(): int { return 0; }",
            "sealed Assert module alias cannot be shadowed");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc test(): int { "
            "try { throw 5; } catch (Assert) { return 1; } finally { } "
            "}",
            "sealed Assert module alias cannot be shadowed");

        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "suspend fnc worker(): void { "
            "val Assert: int = 0; while (true) { Assert = Assert + 1; } }",
            "sealed Assert module alias cannot be shadowed");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "suspend fnc Assert(): void { "
            "val counter: int = 0; while (true) { counter = counter + 1; } }",
            "sealed Assert module alias cannot be shadowed");

        // A verified named-signature importer is still future work: reject
        // any named arguments rather than binding source parameters by guess.
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert::equal(a: 1, b: 2); } "
            "pub fnc main(): int { return 0; }",
            "native sealed Assert named arguments require verified typed signature");

        // Source 'is' is reference identity, not tagged scalar 'eq'.
        // Reject until native ownership and reference identity are lowered.
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert.same(1, 1); } "
            "pub fnc main(): int { return 0; }",
            "native sealed Assert identity requires reference-identity ABI lowering");
        expect_rejected(
            "import module Assert from \"std/assert\"; "
            "pub fnc check(): bool { return Assert::same_throws(1, 1); } "
            "pub fnc main(): int { return 0; }",
            "native sealed Assert identity requires reference-identity ABI lowering");

        std::cout << "NATIVE_ASSERT_IMPORT_PARSER_CONTRACT_PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
