#include "oreslang/frontend/Reachability.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

using namespace oreslang;

static std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Integer;
    expr->integer = value;
    return expr;
}

static std::unique_ptr<Expression> call(const std::string& name) {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Call;
    expr->name = name;
    return expr;
}

static Function fn(const std::string& name, std::unique_ptr<Expression> result, bool exported = false) {
    Function out;
    out.name = name;
    out.result = std::move(result);
    out.exported = exported;
    return out;
}

static bool has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

int main() {
    {
        Program program;
        program.functions.push_back(fn("leaf", integer(1)));
        program.functions.push_back(fn("live", call("leaf")));
        program.functions.push_back(fn("dead", integer(0)));
        program.functions.push_back(fn("main", call("live")));

        const auto result = analyze_reachability(program, {"main"}, false);
        assert(result.roots == std::vector<std::string>{"__root__.main"});
        assert(has(result.retained_symbols, "__root__.main"));
        assert(has(result.retained_symbols, "__root__.live"));
        assert(has(result.retained_symbols, "__root__.leaf"));
        assert(has(result.removed_symbols, "__root__.dead"));
    }

    {
        Program program;
        program.functions.push_back(fn("helper", integer(41)));
        program.functions.push_back(fn("answer", call("helper"), true));
        program.functions.push_back(fn("dead", integer(0)));

        const auto result = analyze_reachability(program, {}, true);
        assert(result.roots == std::vector<std::string>{"__root__.answer"});
        assert(has(result.retained_symbols, "__root__.helper"));
        assert(!has(result.roots, "__root__.helper"));
        assert(has(result.removed_symbols, "__root__.dead"));
    }

    {
        Program program;
        program.functions.push_back(fn("helper", integer(7)));

        ActorDecl worker;
        worker.name = "Worker";
        worker.on_message = fn("on_message", call("helper"));
        program.actors.push_back(std::move(worker));

        Function main = fn("main", integer(0));
        ActorSpawnBinding spawn;
        spawn.name = "worker";
        spawn.actor_type = "Worker";
        main.actor_spawns.push_back(std::move(spawn));
        program.functions.push_back(std::move(main));

        const auto result = analyze_reachability(program, {"main"}, false);
        assert(has(result.retained_symbols, "__root__.Worker"));
        assert(has(result.retained_symbols, "__root__.helper"));
    }

    {
        Program program;
        program.functions.push_back(fn("main", call("missing")));
        bool rejected = false;
        try {
            (void)analyze_reachability(program, {"main"}, false);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "unknown native direct calls must fail closed");
    }

    return 0;
}
