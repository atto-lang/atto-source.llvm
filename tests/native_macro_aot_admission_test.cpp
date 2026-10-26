#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

using oreslang::parse_program;

static void check(bool condition) {
    if (!condition) std::abort();
}

static void denied(std::string_view source) {
    bool rejected = false;
    try {
        (void)parse_program(source);
    } catch (const std::exception& error) {
        const std::string message(error.what());
        rejected = message.find("NATIVE_MACRO_EXPANDER_REQUIRED") != std::string::npos
                && message.find("before LLVM IR") != std::string::npos
                && message.find("runtime fallback is forbidden") != std::string::npos;
    }
    check(rejected);
}

int main() {
    denied(R"(
define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
  return value;
}
pub fnc main(): int {
  return 42;
}
)");

    denied(R"(
define pub macro emit(Type target) -> SyntaxList<Decl> {
  return generated(target);
}
pub fnc main(): int {
  return 42;
}
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
define module demo as
  define macro identity(Syntax<Expr> value) -> Syntax<Expr> {
    return value;
  }
end
)");

    return 0;
}
