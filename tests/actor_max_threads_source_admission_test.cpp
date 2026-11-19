#include "oreslang/frontend/Parser.hpp"
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

using oreslang::parse_program;
static void check(bool condition) { if (!condition) std::abort(); }

static constexpr std::string_view valid_actor = R"(
define actor Worker as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
)";
static constexpr std::string_view canonical_enum = R"(
define enum MaxActorThreads as
 LOW int = 1
 MEDIUM int = 2
 HIGH int = 3
end
)";
static void denied(std::string src, const char* diagnostic) {
    bool rejected = false;
    try { (void)parse_program(src); }
    catch (const std::exception& e) {
        rejected = std::string(e.what()).find(diagnostic) != std::string::npos;
    }
    check(rejected);
}

int main() {
    auto plain = parse_program(valid_actor);
    check(plain.actors.size() == 1);
    check(plain.actors.front().max_actor_threads == oreslang::MaxActorThreads::LOW);

    auto low = parse_program(std::string(canonical_enum)
            + "@ActorMaxThreads(MaxActorThreads.LOW)\n" + std::string(valid_actor));
    check(low.actors.size() == 1);
    check(low.actors.front().max_actor_threads == oreslang::MaxActorThreads::LOW);

    auto implicit = parse_program(
            "@ActorMaxThreads(MaxActorThreads.LOW)\n" + std::string(valid_actor));
    check(implicit.actors.size() == 1);
    check(implicit.actors.front().max_actor_threads == oreslang::MaxActorThreads::LOW);

    auto module = parse_program(std::string("define module work as\n")
            + std::string(canonical_enum)
            + "@ActorMaxThreads(MaxActorThreads.LOW)\n"
            + std::string(valid_actor) + "end\n");
    check(module.actors.size() == 1);
    check(module.actors.front().max_actor_threads == oreslang::MaxActorThreads::LOW);

    // No native supervisor source/CONTROL lowering exists. Explicitly
    // reject each nominal composition form before an ordinary actor/class
    // fallback or native LLVM IR emission can claim executable parity.
    for (const auto* source : {
        "define supervisor S1 as on_panic(f: Failure): void { return; } end",
        "pub define supervisor S1 as end",
        "define pub supervisor S1 as end",
        "define supervisor actor S1 as end",
        "define supervisor S1 extends Actor as end",
        "define supervisor S1 implements Observer as end",
        "define supervisor S1 with RestartPolicy as end",
        "define module Work as define supervisor S1 as end end"
    }) {
        denied(source, "NATIVE_SUPERVISOR_UNSUPPORTED");
    }

    for (const auto* tier : {"MEDIUM", "HIGH"}) {
        denied(std::string(canonical_enum)
                   + "@ActorMaxThreads(MaxActorThreads." + tier + ")\n"
                   + std::string(valid_actor),
               "per-turn actor state isolation");
    }
    denied(std::string(canonical_enum) + std::string(canonical_enum)
            + std::string(valid_actor), "duplicate MaxActorThreads");
    denied("define enum MaxActorThreads as LOW int = 2 MEDIUM int = 2 HIGH int = 3 end\n"
           + std::string(valid_actor), "canonical integer");
    denied("define enum MaxActorThreads as LOW int = 1 MEDIUM int = 2 HIGH int = 4 end\n"
           + std::string(valid_actor), "canonical integer");
    denied("define enum OtherThreads as LOW int = 1 MEDIUM int = 2 HIGH int = 3 end\n"
           + std::string(valid_actor), "expected 'MaxActorThreads'");
    denied("@ActorMaxThreads(MaxActorThreads.EXTREME)\n"
           + std::string(valid_actor), "requires MaxActorThreads.LOW/MEDIUM/HIGH");
    denied("@ActorMaxThreads(3)\n" + std::string(valid_actor),
           "expected 'MaxActorThreads'");
    denied("@ActorMaxThreads(MaxActorThreads.LOW)\n"
           "@ActorMaxThreads(MaxActorThreads.LOW)\n" + std::string(valid_actor),
           "expected 'define'");
    denied("@ActorMaxThreads(MaxActorThreads.LOW)\n"
           "define class C as let int x = 1; end\n"
           + std::string(valid_actor), "expected 'actor'");
    return 0;
}
