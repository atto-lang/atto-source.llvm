#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

static void must_reject(std::string_view source, std::string_view expected) {
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos) {
            return;
        }
    }
    std::abort();
}

int main() {
    // A valid sealed-state declaration reaches semantic lowering admission,
    // not the old scalar actor field initializer or implicit LLVM layout.
    must_reject(R"(
        define actor Counter as
            state {
                value: int = 0
                other: int = 2
            }
        end
    )", "sealed actor state AST is recognized");

    must_reject(
        "define actor Counter as state { v: int = 0; v: int = 1; } end",
        "duplicate or reserved sealed actor state field");
    must_reject(
        "define actor Counter as state { state: int = 0; } end",
        "duplicate or reserved sealed actor state field");
    must_reject(
        "define actor Counter as state { value: int; } end",
        "expected '='");
    must_reject(
        "define actor Counter as state { first: int = 1 second: int = 2 } end",
        "semicolon or newline");
    must_reject(
        "define actor Counter as let state: int = 2; "
        "pub on_message(int n): int { return n; } end",
        "reserved runtime-owned storage");
    must_reject(
        "define actor Counter as state { value: String = 0; } end",
        "expected 'int'");
    must_reject(
        "define actor Counter as state { v: int = 0; } state { w: int = 1; } end",
        "duplicate sealed actor state block");
    must_reject(
        "define actor Counter as state { value: int = 0; } pub get(): int { return value; } end",
        "state methods require sealed handle_state");

    // An ordinary source function must remain unaffected by this new AST.
    auto program = oreslang::parse_program("pub fnc main(): int { return 0; }");
    if (program.functions.size() != 1 || !program.actors.empty()) std::abort();
    return 0;
}
