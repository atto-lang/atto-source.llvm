#include "oreslang/frontend/CheckedI64Directive.hpp"

#include <cstdint>
#include <string>
#include <stdexcept>
#include <string_view>

using oreslang::checked_i64_source_opt_in;

static void require(bool condition) {
    if (!condition) throw std::runtime_error("checked-i64-v1 directive regression");
}

static void rejected(std::string_view text) {
    bool threw = false;
    try {
        (void)checked_i64_source_opt_in(text);
    } catch (const std::runtime_error& e) {
        threw = std::string_view(e.what()).find("checked-i64-v1") !=
                std::string_view::npos;
    }
    require(threw);
}

int main() {
    constexpr std::string_view valid =
        "// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }\n";
    require(checked_i64_source_opt_in(valid));
    require(!checked_i64_source_opt_in("pub fnc main(): int { return 43; }\n"));
    // A string literal mentioning the directive is not a directive line.
    require(!checked_i64_source_opt_in(
        "pub fnc demo(): int { return 43; } // oreslang:checked-i64-v1\n"));
    rejected("// oreslang:checked-i64-v1");
    rejected("// oreslang:checked-i64-v1\r\npub fnc main(): int { return 43; }");
    rejected(" // oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("\r// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("\v// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("\f// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("// oreslang:checked-i64-v1\n\f// oreslang:checked-i64-v1\n");
    rejected("\n// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("// oreslang:checked-i64-v2\npub fnc main(): int { return 43; }");
    rejected("// oreslang:checked-i64-v1 extra\npub fnc main(): int { return 43; }");
    rejected("// oreslang:checked-i64-v1\n// oreslang:checked-i64-v1\n");
    rejected("\xEF\xBB\xBF// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }");
    rejected("pub fnc main(): int { return 43; }\n// oreslang:checked-i64-v1\n");

    // This source is UNMARKED: text that resembles a directive inside a
    // block comment is not a line-comment token and must not opt in or fail.
    require(!checked_i64_source_opt_in(
        "/*\\n// oreslang:checked-i64-v1\\n*/\\n"
        "pub fnc main(): int { return 43; }\\n"));
    require(!checked_i64_source_opt_in(
        "/* outer /* nested\\n// oreslang:checked-i64-v2\\n"
        "*/ still outer */\\npub fnc main(): int { return 43; }\\n"));
    require(!checked_i64_source_opt_in(
        "pub fnc main(): int { return 43; }\\n"
        "/*\\n // oreslang:checked-i64-v1\\n */\\n"));
    require(checked_i64_source_opt_in(
        "// oreslang:checked-i64-v1\\n"
        "/*\\n// oreslang:checked-i64-v1\\n*/\\n"
        "pub fnc main(): int { return 43; }\\n"));
    // Quoted material also must not be interpreted as a directive.
    require(!checked_i64_source_opt_in(
        "\\\"// oreslang:checked-i64-v1\\\"\\n"
        "pub fnc main(): int { return 43; }\\n"));
    // But genuine line comments after block close/code remain malformed.
    rejected("/* harmless */ // oreslang:checked-i64-v1\\n");
    rejected("/* harmless */\\n// oreslang:checked-i64-v1\\n");
    rejected("pub fnc main(): int { return 43; }\\n"
             "// oreslang:checked-i64-v2\\n");

    // Validate state independence and displaced directives even in long
    // comment sequences. This must run in release builds with -DNDEBUG.
    std::uint64_t seed = 20261015u;
    for (int i = 0; i < 4096; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const std::string innocent = "// unrelated-" + std::to_string(seed) + "\n";
        require(checked_i64_source_opt_in(
            std::string("// oreslang:checked-i64-v1\n") + innocent +
            "pub fnc main(): int { return 43; }\n"));
        require(!checked_i64_source_opt_in(
            innocent + "pub fnc main(): int { return 43; }\n"));
        rejected(innocent + std::string(valid));
        rejected(std::string("// oreslang:checked-i64-v1\n") +
                 innocent + std::string(valid));
    }
}
