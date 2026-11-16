#include "oreslang/types/StaticSafetyAnalysis.hpp"

#include <cassert>
#include <memory>
#include <string>

using namespace oreslang;

static bool has_rule(const std::vector<NativeStaticSafetyFinding>& values,
                     const std::string& rule) {
    for (const auto& value : values) if (value.rule == rule) return true;
    return false;
}

static Function recursive_function() {
    Function fn;
    fn.name = "again";
    fn.result = std::make_unique<Expression>();
    fn.result->kind = Expression::Kind::Call;
    fn.result->name = "again";
    return fn;
}

static ActorDecl worker(bool send_to_spawned) {
    ActorDecl actor;
    actor.name = "Worker";
    actor.on_message.name = "on_message";
    ActorSpawnBinding spawn;
    spawn.name = "child";
    spawn.actor_type = "Worker";
    actor.on_message.actor_spawns.push_back(std::move(spawn));
    if (send_to_spawned) {
        ActorSend send;
        send.reference = "child";
        actor.on_message.actor_sends.push_back(std::move(send));
    }
    return actor;
}

int main() {
    {
        Program p;
        p.functions.push_back(recursive_function());
        const auto result = analyze_native_static_safety(p);
        assert(has_rule(result, "ORES-AMP-004"));
        assert(!has_rule(result, "ORES-AMP-001"));
    }
    {
        Program p;
        p.actors.push_back(worker(true));
        const auto result = analyze_native_static_safety(p);
        assert(has_rule(result, "ORES-AMP-001"));
        assert(!has_rule(result, "ORES-AMP-004"));
    }
    {
        Program p;
        p.actors.push_back(worker(false));
        const auto result = analyze_native_static_safety(p);
        // Spawning without messaging is not immediate handler execution.
        assert(!has_rule(result, "ORES-AMP-001"));
    }
    {
        Program p;
        Function fn;
        fn.name = "bounded";
        fn.scalar_while = std::make_unique<ScalarWhileLoop>();
        fn.scalar_while->counter = "i";
        fn.scalar_while->initial = 0;
        fn.scalar_while->upper_bound = 12;
        p.functions.push_back(std::move(fn));
        assert(!has_rule(analyze_native_static_safety(p), "ORES-TERM-002"));
    }
    {
        Program p;
        CooperativeScalarLoop loop;
        loop.name = "cooperative_spin";
        p.cooperative_loops.push_back(std::move(loop));
        assert(has_rule(analyze_native_static_safety(p), "ORES-TERM-002"));
    }
    {
        Program p;
        ActorDecl actor;
        actor.name = "Worker";
        actor.on_message.name = "on_message";
        ActorSend send;
        send.reference = "external";
        actor.on_message.actor_sends.push_back(std::move(send));
        p.actors.push_back(std::move(actor));
        assert(has_rule(analyze_native_static_safety(p), "ORES-ANALYSIS-001"));
    }
    {
        Program p;
        Function fn = recursive_function();
        // A wide expression avoids a test-only deep recursive AST. The
        // scanner must reject overflow rather than omit the final call.
        for (std::size_t i = 0; i < 30000; ++i) {
            auto call = std::make_unique<Expression>();
            call->kind = Expression::Kind::Call;
            call->name = "again";
            fn.result->arguments.push_back(std::move(call));
        }
        p.functions.push_back(std::move(fn));
        bool rejected = false;
        try {
            (void)analyze_native_static_safety(p);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) ==
                "native static safety graph budget exceeded";
        }
        assert(rejected);
    }
    return 0;
}
