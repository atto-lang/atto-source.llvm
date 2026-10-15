#include "oreslang/frontend/CheckedI64Directive.hpp"

#include <cassert>
#include <stdexcept>
#include <string_view>

using oreslang::checked_i64_source_opt_in;

static void rejected(std::string_view text) {
    bool threw = false;
    try {
        (void)checked_i64_source_opt_in(text);
    } catch (const std::runtime_error& e) {
        threw = std::string_view(e.what()).find("checked-i64-v1") !=
                std::string_view::npos;
    }
    assert(threw && "malformed checked directive must fail closed");
}

int main() {
    constexpr std::string_view valid =
        "// oreslang:checked-i64-v1\npub fnc main(): int { return 43; }\n";
    assert(checked_i64_source_opt_in(valid));
    assert(!checked_i64_source_opt_in("pub fnc main(): int { return 43; }\n"));
    // A string literal mentioning the directive is not a directive line.
    assert(!checked_i64_source_opt_in(
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
}
