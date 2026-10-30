#include "oreslang/frontend/Parser.hpp"
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

using oreslang::parse_program;

static void check(bool condition) { if (!condition) std::abort(); }

static void denied(std::string_view source) {
    bool rejected = false;
    try { (void)parse_program(source); }
    catch (const std::exception& error) {
        const std::string message(error.what());
        rejected = message.find("ORES-SINGLETON-UNSUPPORTED") != std::string::npos &&
                   message.find("five-backend guest execution parity") != std::string::npos;
    }
    check(rejected);
}

int main() {
    denied("define singleton Settings as end\n");
    denied("define pub singleton Settings as end\n");
    denied("pub define singleton Settings as end\n");
    denied("define module app as\n define singleton Settings as end\nend\n");

    // singleton is contextual: ordinary callable names and comments stay legal.
    const auto program = parse_program(
        "// define singleton CommentOnly as end\n"
        "fnc singleton(): int { return 42; }\n"
        "fnc singleton_count(): int { return singleton(); }\n"
        "fnc main(): int { return singleton_count(); }\n");
    check(!program.functions.empty());
    return 0;
}
