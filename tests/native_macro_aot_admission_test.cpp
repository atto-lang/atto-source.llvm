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
    // Group the sites 16 x 8 so the sum stays within the native expression
    // depth limit (64); a flat 128-term '+' chain is rejected for depth,
    // which would mask the expansion budget this case is about.
    std::string uses;
    for (int group = 0; group < 16; ++group) {
        if (!uses.empty()) uses += "+";
        uses += "(";
        for (int i = 0; i < 8; ++i) {
            if (i) uses += "+";
            uses += "identity!(syntax expr 1)";
        }
        uses += ")";
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
