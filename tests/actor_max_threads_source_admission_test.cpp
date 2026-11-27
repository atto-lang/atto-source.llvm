#include "oreslang/frontend/Parser.hpp"
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

using oreslang::parse_program;
#define check(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "actor admission failure line %d: %s\n", __LINE__, #condition); \
    std::abort(); } } while (false)

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
        if (!rejected) std::fprintf(stderr, "expected %s, observed %s\n", diagnostic, e.what());
    }
    if (!rejected) std::fprintf(stderr, "unexpected admission/diagnostic: %s\n", src.c_str());
    check(rejected);
}

int main() {
    auto plain = parse_program(valid_actor);
    check(plain.actors.size() == 1);
    check(plain.actors.front().max_actor_threads == oreslang::MaxActorThreads::LOW);
    check(!plain.actors.front().declared_receives_i64);

    // Native actor messages are permanently int-only in this stage-0 ABI.
    // The explicit receives clause must survive in the parsed AST.
    std::string typed_header(valid_actor);
    const auto at = typed_header.find("Worker as");
    check(at != std::string::npos);
    typed_header.replace(at, std::string("Worker as").size(), "Worker receives int as");
    const auto typed_actor = parse_program(typed_header);
    check(typed_actor.actors.size() == 1);
    check(typed_actor.actors.front().declared_receives_i64);

    auto changed_header = [&](const std::string& replacement) {
        std::string source(valid_actor);
        const auto pos = source.find("Worker as");
        check(pos != std::string::npos);
        source.replace(pos, std::string("Worker as").size(), replacement);
        return source;
    };
    denied(changed_header("Worker receives String as"),
           "NATIVE_ACTOR_RECEIVES_TYPE_UNSUPPORTED");
    denied(changed_header("Worker receives int | String as"),
           "NATIVE_ACTOR_RECEIVES_UNION_UNSUPPORTED");
    denied(changed_header("Worker receives int|int as"),
           "NATIVE_ACTOR_RECEIVES_UNION_UNSUPPORTED");
    denied(changed_header("Worker receives int /* separator */ | int as"),
           "NATIVE_ACTOR_RECEIVES_UNION_UNSUPPORTED");
    denied(changed_header("Worker receives int receives int as"),
           "NATIVE_ACTOR_RECEIVES_DUPLICATE");
    denied(changed_header("Worker sends int as"),
           "NATIVE_ACTOR_SENDS_EFFECT_UNSUPPORTED");
    denied(changed_header("Worker receives int sends int as"),
           "NATIVE_ACTOR_SENDS_EFFECT_UNSUPPORTED");

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
    // Grouped or legacy spawn policies must not become plain ActorRef bindings.
    for (const auto* suffix : {
        "with {}",
        "with { schedule { max_parallelism = 2; affinity = sticky; } }",
        "with { resources { heap_size = 67108864; stack_size = 65536; mailbox_capacity = 1024; } }",
        "with { init { name = \"worker-1\"; seed = 42; } }",
        "/* comment */ with { init {} }",
        "with schedule(max_parallelism=2)",
        "with { unknown {} }"
    }) {
        denied(std::string(valid_actor) + "\n pub fnc main(): int { val w: ActorRef<Worker> = spawn Worker() " +
               suffix + "; return 0; }", "E_SPAWN_OPTIONS_UNSUPPORTED");
    }
    auto legacy_spawn = parse_program(std::string(valid_actor) +
        "\n pub fnc main(): int { val w: ActorRef<Worker> = spawn Worker(); return 0; }");
    check(legacy_spawn.actors.size() == 1);
    return 0;
}
