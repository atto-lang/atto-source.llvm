#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
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
static void scalar_42(std::string_view source) {
    auto program = parse_program(source);
    check(program.functions.size() == 1);
    const auto& function = program.functions.front();
    check(function.result != nullptr);
    check(function.result->kind == Expression::Kind::Integer);
    check(function.result->integer == 42);
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
    // A balanced tree keeps source/parser nesting shallow while trying to
    // amplify compile-time AST work beyond the strict 2048-node budget.
    std::string broad = "1";
    for (int i = 0; i < 12; ++i) broad = "(" + broad + "+" + broad + ")";
    const std::string bounded =
        "define macro literal(Const<int> value) -> Syntax<Expr> { return quote(value); }"
        "pub fnc main(): int { return literal!(const " + broad + "); }";
    denied(bounded);

    return 0;
}
