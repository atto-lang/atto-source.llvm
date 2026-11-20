#include "oreslang/frontend/Parser.hpp"
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

static void check(bool value) { if (!value) std::abort(); }

static void denied(std::string_view source, std::string_view code) {
    try {
        (void)oreslang::parse_program(source);
        std::abort();
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(code) != std::string_view::npos);
    }
}

int main() {
    static constexpr std::string_view gpu_diagnostic = "GPU_ACTOR_MIR_UNSUPPORTED";
    // Both file-level and module-wrapped declarations reject before ordinary
    // ActorDecl is constructed or native LLVM IR may be emitted.
    for (const auto* source : {
        "define gpu actor Particle as state {} end",
        "define gpu actor Particle as state { let int x: 0; } end",
        "define pub gpu actor Particle as state {} end",
        "pub define gpu actor Particle as state {} end",
        "define module Sim as define gpu actor Particle as state {} end end",
        "define gpu actor Particle as state {} foo() -> int :: return 1; end end"
    }) denied(source, gpu_diagnostic);

    // Generic non-declaration domain syntax must not become a GPU actor,
    // and must still be rejected before any implicit host codegen.
    denied("fnc gpu_kernel(): int { gpu task { 42 }; return 0; }",
           "native gpu/quantum boundary unsupported");

    // GPU tokens in comments are not declarations.
    auto normal = oreslang::parse_program(R"(
        fnc main(): int {
            // define gpu actor NotAnActor as state {} end
            return 42;
        }
    )");
    check(normal.actors.empty());
    check(normal.functions.size() == 1);

    // Ordinary actor admission must not be downgraded into GPU rejection.
    auto actor = oreslang::parse_program(R"(
        define actor Worker as {
            let total: int = 0;
            pub fnc on_message(int n): int {
                total = total + n;
                return total;
            }
        }
    )");
    check(actor.actors.size() == 1);
    return 0;
}
