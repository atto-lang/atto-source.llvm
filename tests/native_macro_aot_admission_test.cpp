#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

using oreslang::parse_program;
using oreslang::Expression;

static void check(bool condition) {
    if (!condition) std::abort();
}
static void denied(std::string_view source) {
    bool rejected = false;
    try {
        (void)parse_program(source);
    } catch (const std::exception& error) {
        const std::string message(error.what());
        rejected = (message.find("NATIVE_MACRO_EXPANDER_REQUIRED") != std::string::npos ||
                    message.find("NATIVE_MACRO_CONST_") != std::string::npos ||
                    message.find("NATIVE_MACRO_BUDGET") != std::string::npos)
                && message.find("runtime fallback is forbidden") != std::string::npos;
    }
    check(rejected);
}
static void scalar_value(std::string_view source, std::int64_t expected) {
    auto program = parse_program(source);
    check(program.functions.size() == 1);
    const auto& function = program.functions.front();
    check(function.result != nullptr);
    check(function.result->kind == Expression::Kind::Integer);
    check(function.result->integer == expected);
}
static void scalar_42(std::string_view source) {
    scalar_value(source, 42);
}
int main() {
    // Parser-level typed AST contract: this is NOT source admission evidence.
    // The production ModuleLoader must independently derive this binding from
    // a descriptor-verified producer and cryptographically hash its bytes.
    const auto producer_exports = oreslang::parse_native_public_macro_exports(R"(
define pub macro literal(Const<int> value) -> Syntax<Expr> {
    return quote(value);
}
)");
    check(producer_exports.size() == 1);
    check(producer_exports.front().name == "literal");
    check(producer_exports.front().kind == oreslang::NativeMacroKind::ConstIntQuote);
    const auto macro_consumer = R"(
import * as lib from "./producer.atto";
pub fnc main() -> int { return lib.literal!(const 40+2); }
)";
    const auto declared_imports =
        oreslang::scan_native_macro_import_headers(macro_consumer);
    check(declared_imports.size() == 1);
    check(declared_imports.front().alias == "lib");
    check(declared_imports.front().path == "./producer.atto");
    const std::vector<oreslang::NativeMacroBinding> verified_test_bindings{{
        "lib", "./producer.atto", "project/producer.atto",
        std::string(64, 'a'), producer_exports
    }};
    auto imported = oreslang::parse_unresolved_program(
        macro_consumer, false, false, verified_test_bindings);
    check(imported.imports.empty());
    check(imported.functions.size() == 1);
    check(imported.functions.front().result != nullptr);
    check(imported.functions.front().result->kind == Expression::Kind::Integer);
    check(imported.functions.front().result->integer == 42);
    check(imported.compile_time_macro_imports.size() == 1);
    check(imported.compile_time_macro_imports.front().logical_owner ==
          "project/producer.atto");
    check(imported.compile_time_macro_imports.front().source_sha256 ==
          std::string(64, 'a'));
    {
        bool denied_without_owner = false;
        try {
            (void)oreslang::parse_unresolved_program(macro_consumer);
        } catch (const std::exception& ex) {
            denied_without_owner = std::string(ex.what()).find(
                "NATIVE_IMPORTED_MACRO_LINKER_REQUIRED") != std::string::npos;
        }
        check(denied_without_owner);
    }

    scalar_42(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(): int {
  return literal!(const 40 + 2);
}
)");
    scalar_42(R"(
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
pub fnc main(): int {
  return identity!(syntax expr 42);
}
)");
    scalar_42(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
pub fnc main(): int {
  return identity!(syntax expr literal!(const 6 * 7));
}
)");
    // Canonical expand! syntax: same typed scalar expansion before LLVM IR.
    scalar_42(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(): int { return expand! literal(const 6 * 7); }
)");
    scalar_42(R"(
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
pub fnc main(): int { return expand! identity(syntax expr 42); }
)");
    denied(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(int input): int { return expand! literal(const input); }
)");
    denied(R"(
pub fnc main(): int { return expand! missing(const 42); }
)");
    denied(R"(
expand! missing(type User);
pub fnc main(): int { return 42; }
)");

    // Compiler admission rejects unsafe and unsupported macro forms before
    // any LLVM IR can be emitted. No source-string substitution or JIT.
    denied(R"(
define macro emit(Type target) -> SyntaxList<Decl> {
  return generated(target);
}
pub fnc main(): int { return 42; }
)");
    denied(R"(
define pub macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(): int { return 42; }
)");
    denied(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return eval(value);
}
pub fnc main(): int { return literal!(const 42); }
)");
    denied(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(int input): int { return literal!(const input); }
)");
    denied(R"(
define macro literal(Const<int> value) -> Syntax<Expr> {
  return quote(value);
}
pub fnc main(): int { return literal!(const 9223372036854775807 + 1); }
)");
    denied(R"(
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return quote(value + value);
}
pub fnc main(): int { return identity!(syntax expr 21); }
)");
    denied(R"(
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
pub fnc main(): int { return identity!(syntax expr 42); }
)");
    denied(R"(
pub fnc main(): int {
  return identity!(syntax expr 42);
}
)");
    denied(R"(
identity!(syntax expr 42);
pub fnc main(): int {
  return 42;
}
)");
    denied(R"(
Producer.emit!(type int);
pub fnc main(): int { return 42; }
)");
    denied(R"(
define module demo as
  define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
    return value;
  }
end
)");
    // Inclusive shared const-v1 evaluator limit: 256 leaves form 511 AST
    // nodes and must succeed. Doubling the tree has 1023 AST nodes and must
    // reject (before IR), independently of expression-parser depth.
    const std::string prefix =
        "define macro literal(Const<int> value) -> Syntax<Expr> { return quote(value); }"
        "pub fnc main(): int { return literal!(const ";
    std::string broad = "1";
    for (int i = 0; i < 8; ++i) broad = "(" + broad + "+" + broad + ")";
    scalar_value(prefix + broad + "); }", 256);
    denied(prefix + "(" + broad + "+" + broad + ")" + "); }");

    // Compiler intrinsics are not shadowable, even by never-invoked macro
    // declarations. These are typed compile-time denials, not ordinary calls.
    denied(R"(
define macro quote(Const<int> value) -> Syntax<Expr> { return quote(value); }
pub fnc main(): int { return 42; }
)");
    denied(R"(
define macro literal(Const<int> quote) -> Syntax<Expr> { return quote(quote); }
pub fnc main(): int { return 42; }
)");
    denied(R"(
define macro identity(Syntax<Expr> unquote) -> Syntax<Expr> { return unquote; }
pub fnc main(): int { return 42; }
)");
    denied(R"(
define macro literal(Const<int> value) -> Syntax<Decl> { return quote(value); }
pub fnc main(): int { return 42; }
)");
    denied(R"(
define macro literal(Const<int> first, Const<int> second) -> Syntax<Expr> {
  return quote(first);
}
pub fnc main(): int { return 42; }
)");

    // Shared scalar profile: eight declarations per translation unit.
    const std::string identity_body =
        "(Syntax<Expr> value) -> Syntax<Expr> { return value; }";
    std::string definitions;
    for (int i = 0; i < 8; ++i) {
        definitions += "define macro ident_" + std::to_string(i) + identity_body;
    }
    scalar_42(definitions + "pub fnc main(): int { return 42; }");
    denied(definitions + "define macro ident_8" + identity_body
                   + "pub fnc main(): int { return 42; }");

    // All 128 independent call sites must succeed; the 129th must fail before
    // executable LLVM IR. This is not a nested-recursion depth check.
    const std::string identity_def =
        "define macro identity(Syntax<Expr> value) -> Syntax<Expr> { return value; }";
    std::string uses;
    for (int i = 0; i < 128; ++i) {
        if (!uses.empty()) uses += "+";
        uses += "identity!(syntax expr 1)";
    }
    {
        auto program = parse_program(
            identity_def + "pub fnc main(): int { return " + uses + "; }");
        check(program.functions.size() == 1);
        check(program.functions.front().result != nullptr);
    }
    denied(identity_def + "pub fnc main(): int { return "
                   + uses + "+identity!(syntax expr 1); }");

    // The closed-constant evaluator admits AST depth 64 inclusive and rejects
    // depth 65 with NATIVE_MACRO_BUDGET. The macro-input parser must allow the
    // evaluator to issue the decisive bounded-rejection diagnostic.
    const std::string literal_def =
        "define macro literal(Const<int> value) -> Syntax<Expr> { return quote(value); }";
    std::string negatives;
    for (int i = 0; i < 64; ++i) negatives += "- ";
    scalar_value(literal_def
        + "pub fnc main(): int { return literal!(const "
        + negatives + "1); }", 1);
    denied(literal_def
        + "pub fnc main(): int { return literal!(const "
        + negatives + "- 1); }");

    return 0;
}
