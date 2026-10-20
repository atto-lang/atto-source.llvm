#include "oreslang/frontend/Reachability.hpp"

#include <algorithm>
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

static std::unique_ptr<Expression> call_arity(
        const std::string& name, std::size_t arity) {
    auto expr = call(name);
    for (std::size_t i = 0; i < arity; ++i)
        expr->arguments.push_back(integer(static_cast<std::int64_t>(i + 1)));
    return expr;
}

static Function fn_arity(
        const std::string& name,
        std::size_t arity,
        std::unique_ptr<Expression> result,
        bool exported = false) {
    Function out = fn(name, std::move(result), exported);
    for (std::size_t i = 0; i < arity; ++i) {
        Parameter parameter;
        parameter.name = "p" + std::to_string(i);
        out.parameters.push_back(std::move(parameter));
    }
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

        auto shaken = tree_shake_program(std::move(program), {"main"}, false);
        const auto& result = shaken.reachability;
        assert(result.roots == std::vector<std::string>{"__root__.main"});
        assert(has(result.retained_symbols, "__root__.main"));
        assert(has(result.retained_symbols, "__root__.live"));
        assert(has(result.retained_symbols, "__root__.leaf"));
        assert(has(result.removed_symbols, "__root__.dead"));

        assert(shaken.program.functions.size() == 3);
        assert(std::none_of(shaken.program.functions.begin(),
                            shaken.program.functions.end(),
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
        Function dead = fn("dead_array_owner", integer(0));
        LocalBinding dead_array;
        dead_array.name = "items";
        dead_array.int_array = true;
        dead.locals.push_back(std::move(dead_array));
        program.functions.push_back(std::move(dead));
        program.functions.push_back(fn("main", integer(42)));

        const auto before = analyze_runtime_features(program, false, false);
        assert(before.contains(RuntimeFeature::IntArrayRuntime));

        auto shaken = tree_shake_program(std::move(program), {"main"}, false);
        const auto after = analyze_runtime_features(shaken.program, false, false);
        assert(!after.contains(RuntimeFeature::IntArrayRuntime));
        assert(after.names().empty());
        assert(shaken.runtime_features == after);
        assert(shaken.runtime_features.names().empty());
    }

    {
        Program program;
        Function live = fn("main", integer(42));
        LocalBinding live_array;
        live_array.name = "items";
        live_array.int_array = true;
        live.locals.push_back(std::move(live_array));
        program.functions.push_back(std::move(live));

        auto shaken = tree_shake_program(std::move(program), {"main"}, false);
        const auto features = analyze_runtime_features(shaken.program, false, false);
        assert(features.names() == std::vector<std::string>{"int-array-runtime"});
        assert(shaken.runtime_features == features);
    }

    {
        Program program;
        Function main = fn("main", integer(0));
        LocalBinding owned;
        owned.name = "owned";
        owned.heap_owned = true;
        owned.initializer = integer(7);
        main.locals.push_back(std::move(owned));
        program.functions.push_back(std::move(main));

        const auto features = analyze_runtime_features(program, false, false);
        assert(features.contains(RuntimeFeature::OwnedI64Region));
        assert(!features.contains(RuntimeFeature::ActorNativeRuntime));
        assert(!features.contains(RuntimeFeature::IntArrayRuntime));
        assert(features.names() == std::vector<std::string>{"owned-i64-region"});
    }

    {
        Program program;
        Function main = fn("main", integer(0));
        LocalBinding array;
        array.name = "items";
        array.int_array = true;
        main.locals.push_back(std::move(array));
        program.functions.push_back(std::move(main));

        const auto features = analyze_runtime_features(program, false, false);
        assert(features.contains(RuntimeFeature::IntArrayRuntime));
        assert(!features.contains(RuntimeFeature::OwnedI64Region));
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";
        actor.on_message = fn("on_message", integer(0));
        program.actors.push_back(std::move(actor));

        const auto native = analyze_runtime_features(program, false, false);
        const auto wasm = analyze_runtime_features(program, true, false);
        assert(native.contains(RuntimeFeature::ActorNativeRuntime));
        assert(!wasm.contains(RuntimeFeature::ActorNativeRuntime));

        bool rejected = false;
        try {
            validate_runtime_features(
                    program, false, false, RuntimeFeatureSet{});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "runtime feature report drift must fail closed");
    }

    {
        Program malformed;
        Function main = fn("main", integer(0));
        LocalBinding local;
        local.name = "ghost";
        local.class_name = "MissingClass";
        local.new_class = true;
        main.locals.push_back(std::move(local));
        malformed.functions.push_back(std::move(main));

        bool rejected = false;
        try {
            (void)analyze_runtime_features(malformed, false, false);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "runtime feature analysis must fail closed on unknown class");
    }

    {
        Program native_program;
        ActorDecl native_actor;
        native_actor.name = "Worker";
        native_actor.on_message = fn("on_message", integer(0));
        native_program.actors.push_back(std::move(native_actor));
        auto native_shaken = tree_shake_program(
                std::move(native_program), {}, false, {"Worker"});
        assert(native_shaken.runtime_features.contains(
                RuntimeFeature::ActorNativeRuntime));

        Program wasm_program;
        ActorDecl wasm_actor;
        wasm_actor.name = "Worker";
        wasm_actor.on_message = fn("on_message", integer(0));
        wasm_program.actors.push_back(std::move(wasm_actor));
        auto wasm_shaken = tree_shake_program(
                std::move(wasm_program), {}, false, {"Worker"}, {}, true, false);
        assert(!wasm_shaken.runtime_features.contains(
                RuntimeFeature::ActorNativeRuntime));
    }

    {
        Program program;
        program.functions.push_back(fn_arity("pick", 1, integer(1)));
        program.functions.push_back(fn_arity("pick", 2, integer(2)));
        program.functions.push_back(fn_arity("pick", 3, integer(3)));

        auto sum = std::make_unique<Expression>();
        sum->kind = Expression::Kind::Add;
        sum->left = call_arity("pick", 1);
        sum->right = call_arity("pick", 2);
        program.functions.push_back(fn("main", std::move(sum)));

        auto shaken = tree_shake_program(std::move(program), {"main"}, false);
        assert(has(shaken.reachability.retained_symbols,
                   "__root__.pick$arity1"));
        assert(has(shaken.reachability.retained_symbols,
                   "__root__.pick$arity2"));
        assert(has(shaken.reachability.removed_symbols,
                   "__root__.pick$arity3"));
        assert(shaken.program.functions.size() == 3);
        assert(std::none_of(
                shaken.program.functions.begin(),
                shaken.program.functions.end(),
                [](const Function& item) {
                    return item.name == "pick" &&
                           item.parameters.size() == 3;
                }));

        bool ambiguous = false;
        try {
            Program roots;
            roots.functions.push_back(fn_arity("pick", 1, integer(1)));
            roots.functions.push_back(fn_arity("pick", 2, integer(2)));
            (void)analyze_reachability(roots, {"pick"}, false);
        } catch (const std::runtime_error&) {
            ambiguous = true;
        }
        assert(ambiguous && "bare overloaded root must remain ambiguous");
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
