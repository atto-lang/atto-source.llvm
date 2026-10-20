#include "oreslang/frontend/Reachability.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

static void expect_rejected(const ReachabilityResult& result) {
    bool rejected = false;
    try {
        (void)canonical_reachability_json(result, false);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
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

        auto pruned = prune_program(std::move(program), result);
        assert(pruned.functions.size() == 3);
        assert(std::none_of(pruned.functions.begin(), pruned.functions.end(),
                [](const Function& item) { return item.name == "dead"; }));

        assert(canonical_reachability_json(result, false) ==
               "{\"format\":\"ores-reachability/1\",\"buildKind\":\"executable\","
               "\"roots\":[\"__root__.main\"],"
               "\"retainedSymbols\":[\"__root__.leaf\",\"__root__.live\",\"__root__.main\"],"
               "\"removedSymbols\":[\"__root__.dead\"],"
               "\"compileTimeConstants\":{}}");
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
        program.functions.push_back(fn("class_init", integer(7)));

        ClassDecl box;
        box.name = "Box";
        ClassField field;
        field.name = "value";
        field.initializer_expression = call("class_init");
        box.fields.push_back(std::move(field));
        program.classes.push_back(std::move(box));

        Function main = fn("main", integer(0));
        LocalBinding local;
        local.name = "box";
        local.class_name = "Box";
        local.new_class = true;
        main.locals.push_back(std::move(local));
        program.functions.push_back(std::move(main));

        const auto result = analyze_reachability(program, {"main"}, false);
        assert(has(result.retained_symbols, "__root__.Box"));
        assert(has(result.retained_symbols, "__root__.class_init"));
    }

    {
        Program program;
        program.functions.push_back(fn("ctor_arg", integer(3)));

        ClassDecl box;
        box.name = "Box";
        program.classes.push_back(std::move(box));

        Function main = fn("main", integer(0));
        Statement conditional;
        conditional.kind = Statement::Kind::Conditional;
        conditional.condition_lhs = integer(1);
        conditional.condition_rhs = integer(1);

        Statement scoped;
        scoped.kind = Statement::Kind::ScopedLocal;
        scoped.branch_local = std::make_unique<LocalBinding>();
        scoped.branch_local->name = "box";
        scoped.branch_local->class_name = "Box";
        scoped.branch_local->new_class = true;
        scoped.branch_local->constructor_arguments.push_back(call("ctor_arg"));
        conditional.then_statements.push_back(std::move(scoped));
        main.statements.push_back(std::move(conditional));
        program.functions.push_back(std::move(main));

        const auto result = analyze_reachability(program, {"main"}, false);
        assert(has(result.retained_symbols, "__root__.Box"));
        assert(has(result.retained_symbols, "__root__.ctor_arg"));
    }


    {
        Program program;
        program.functions.push_back(fn("main", integer(0)));
        CooperativeScalarLoop loop;
        loop.name = "ticker";
        loop.variable = "i";
        loop.initial = 0;
        program.cooperative_loops.push_back(std::move(loop));

        const auto result = analyze_reachability(program, {"main"}, false);
        assert(has(result.roots, "__root__.main"));
        assert(has(result.roots, "__root__.ticker"));
        assert(has(result.retained_symbols, "__root__.ticker"));
        assert(!has(result.removed_symbols, "__root__.ticker"));
    }

    {
        Program program;
        program.functions.push_back(fn("Same", integer(0)));
        ClassDecl klass;
        klass.name = "Same";
        program.classes.push_back(std::move(klass));

        bool rejected = false;
        try {
            (void)analyze_reachability(program, {"Same"}, false);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "semantic symbol collisions must fail closed");
    }


    {
        Program program;
        program.functions.push_back(fn("host_helper", integer(9)));

        ActorDecl hosted;
        hosted.name = "Hosted";
        hosted.on_message = fn("on_message", call("host_helper"));
        program.actors.push_back(std::move(hosted));

        const auto result = analyze_reachability(
                program, {}, false, {"Hosted"});
        assert(result.roots == std::vector<std::string>{"__root__.Hosted"});
        assert(has(result.retained_symbols, "__root__.Hosted"));
        assert(has(result.retained_symbols, "__root__.host_helper"));
    }

    {
        Program program;
        program.functions.push_back(fn("class_helper", integer(5)));

        ClassDecl exported;
        exported.name = "ExportedRecord";
        ClassField field;
        field.name = "value";
        field.initializer_expression = call("class_helper");
        exported.fields.push_back(std::move(field));
        program.classes.push_back(std::move(exported));

        const auto result = analyze_reachability(
                program, {}, false, {}, {"ExportedRecord"});
        assert(result.roots == std::vector<std::string>{"__root__.ExportedRecord"});
        assert(has(result.retained_symbols, "__root__.class_helper"));
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


    {
        Program program;
        program.functions.push_back(fn("main", integer(0)));
        program.functions.push_back(fn("dead", integer(1)));

        const auto result = analyze_reachability(program, {"main"}, false);
        ReachabilityResult forged = result;
        forged.retained_symbols.push_back("__root__.ghost");
        std::sort(forged.retained_symbols.begin(), forged.retained_symbols.end());

        bool rejected = false;
        try {
            (void)prune_program(std::move(program), forged);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "pruning must reject manifest/source classification drift");
    }

    {
        expect_rejected(ReachabilityResult{
            {"__root__.main"},
            {"__root__.main", "__root__.live"},
            {"__root__.live"}});
        expect_rejected(ReachabilityResult{
            {"__root__.missing"},
            {"__root__.main"},
            {}});
        expect_rejected(ReachabilityResult{
            {"__root__.main"},
            {"__root__.main", "__root__.main"},
            {}});
        expect_rejected(ReachabilityResult{
            {"__root__.main"},
            {"__root__.main", std::string("\xc0\xaf", 2)},
            {}});
        expect_rejected(ReachabilityResult{
            {"__root__.main"},
            {"__root__.main", "__root__.bad\nname"},
            {}});
        expect_rejected(ReachabilityResult{
            {"__root__.main"},
            {"__root__.main", std::string(1025, 'a')},
            {}});
    }

    return 0;
}
