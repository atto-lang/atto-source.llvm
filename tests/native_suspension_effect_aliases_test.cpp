#include "oreslang/frontend/Parser.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <iostream>

static void rejects(const std::string& source, std::string_view diagnostic) {
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::exception& e) {
        if (std::string_view(e.what()).find(diagnostic) != std::string_view::npos)
            return;
        throw std::runtime_error(std::string("unexpected compiler diagnostic: ") + e.what());
    }
    throw std::runtime_error("unsupported suspension unexpectedly admitted");
}

int main() {
    for (std::string_view spelling : {"suspends", "susp", "\xE2\xA7\x96", "\xE2\x88\x82"}) {
        const std::string effect(spelling);
        rejects("pub " + effect + " fnc main(): int { return 42; }",
                "native suspends declaration requires suspension-aware");
        const auto actor = std::string("define actor Worker as { let total: int = 0; pub ") +
            effect + " mtd on_message(int n): int { let Task<int> child = task { return 21 * 2; }; return await child; } } "
            "pub fnc main(): int { return 0; }";
        rejects(actor, "ATTO_TASK_E_ACTOR_LOWERING_PENDING");
        (void)oreslang::parse_program("pub fnc main(): int { return 42; } // " + effect);
    }
    (void)oreslang::parse_program("pub fnc main(): int { val int suspension = 42; return suspension; }");
    std::cout << "NATIVE_SUSPENSION_ALIAS_ADMISSION_PASS\n";
}
