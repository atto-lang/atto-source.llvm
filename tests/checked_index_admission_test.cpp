#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipChecker.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
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
    try {
        oreslang::validate_i64_ownership(program);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        if (message.find(
                "native checked index read requires certified closed-view typing and ownership")
                == std::string::npos) {
            std::cerr << "unexpected rejection: " << message << '\n';
            return 1;
        }
        return 0;
    }
    fail("unverified native checked index unexpectedly passed ownership admission");
}
