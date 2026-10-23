#include "oreslang/frontend/ReachabilityManifest.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;

namespace {

std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Integer;
    expr->integer = value;
    return expr;
}

std::unique_ptr<Expression> call(
        const std::string& name,
        std::size_t arity = 0) {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Call;
    expr->name = name;
    for (std::size_t i = 0; i < arity; ++i)
        expr->arguments.push_back(integer(
            static_cast<std::int64_t>(i + 1)));
    return expr;
}

Function fn(
        const std::string& name,
        std::unique_ptr<Expression> result,
        bool exported = false) {
    Function out;
    out.name = name;
    out.result = std::move(result);
    out.exported = exported;
    return out;
}

bool contains(
        const std::vector<SemanticDeclarationId>& values,
        const SemanticDeclarationId& value) {
    return std::binary_search(values.begin(), values.end(), value);
}

bool has_edge(
        const SemanticReachabilityResult& result,
        const SemanticDeclarationId& from,
        const SemanticDeclarationId& to,
        SemanticReachabilityReason reason) {
    return std::any_of(
        result.edges.begin(), result.edges.end(),
        [&](const SemanticReachabilityEdge& edge) {
            return edge.from == from &&
                   edge.to == to &&
                   edge.reason == reason;
        });
}

} // namespace

int main() {
    {
        const CallableId id{"work", 2};
        assert(analysis_callable_key(id) == "work/2");
        assert(native_function_slot(id) == "work$arity2");
        assert(parse_analysis_callable_key("work/2") == id);

        bool rejected = false;
        try {
            (void)parse_analysis_callable_key("work/not-a-number");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);

        for (const std::string bad : {"work/02", "work/2/3"}) {
            rejected = false;
            try {
                (void)parse_analysis_callable_key(bad);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            assert(rejected);
        }
    }

    {
        Program program;
        program.functions.push_back(fn("leaf", integer(7)));
        program.functions.push_back(fn("dead", integer(9)));

        auto conditional = std::make_unique<Expression>();
        conditional->kind = Expression::Kind::Conditional;
        conditional->condition = std::make_unique<Expression>();
        conditional->condition->kind = Expression::Kind::Boolean;
        conditional->condition->boolean = true;
        conditional->when_true = call("leaf");
        conditional->when_false = integer(0);
        program.functions.push_back(
            fn("main", std::move(conditional), true));

        const auto result = analyze_semantic_reachability(program);
        const auto main_id =
            function_declaration_id(CallableId{"main", 0});
        const auto leaf_id =
            function_declaration_id(CallableId{"leaf", 0});
        const auto dead_id =
            function_declaration_id(CallableId{"dead", 0});

        assert(contains(result.roots, main_id));
        assert(contains(result.retained, leaf_id));
        assert(contains(result.removed, dead_id));
        assert(has_edge(
            result, main_id, leaf_id,
            SemanticReachabilityReason::DirectCall));

        const auto explanation =
            explain_semantic_reachability(result, leaf_id);
        assert(!explanation.root);
        assert(explanation.incoming_edges.size() == 1);
        assert(explanation.incoming_edges.front().reason ==
               SemanticReachabilityReason::DirectCall);

        auto manifest = make_reachability_manifest_v1(
            program,
            result);
        assert(manifest.roots ==
               std::vector<std::string>{"__root__.main"});
        assert(manifest.retained_symbols ==
               (std::vector<std::string>{
                   "__root__.leaf", "__root__.main"}));
        assert(manifest.removed_symbols ==
               std::vector<std::string>{"__root__.dead"});
        assert(canonical_reachability_manifest_json(manifest) ==
               "{\"format\":\"ores-reachability/1\","
               "\"buildKind\":\"executable\","
               "\"roots\":[\"__root__.main\"],"
               "\"retainedSymbols\":["
               "\"__root__.leaf\",\"__root__.main\"],"
               "\"removedSymbols\":[\"__root__.dead\"],"
               "\"compileTimeConstants\":{}}");

        manifest.compile_time_constants.emplace(
            "__root__.flag", true);
        manifest.compile_time_constants.emplace(
            "__root__.label", std::string("line\n☃"));
        manifest.compile_time_constants.emplace(
            "__root__.limit", std::int64_t{42});
        const auto constant_json =
            canonical_reachability_manifest_json(manifest);
        assert(constant_json.find(
            "\"__root__.flag\":true") != std::string::npos);
        assert(constant_json.find(
            "\"__root__.label\":\"line\\n☃\"") !=
            std::string::npos);
        assert(constant_json.find(
            "\"__root__.limit\":42") != std::string::npos);
    }

    {
        Program program;
        program.functions.push_back(fn("class_init", integer(3)));

        ClassDecl box;
        box.name = "Box";
        ClassField field;
        field.name = "value";
        field.initializer_expression = call("class_init");
        box.fields.push_back(std::move(field));
        program.classes.push_back(std::move(box));

        Function main = fn("main", integer(0), true);
        LocalBinding local;
        local.name = "box";
        local.class_name = "Box";
        local.new_class = true;
        main.locals.push_back(std::move(local));
        program.functions.push_back(std::move(main));

        const auto result = analyze_semantic_reachability(program);
        const auto main_id =
            function_declaration_id(CallableId{"main", 0});
        const auto box_id = class_declaration_id("Box");
        const auto init_id =
            function_declaration_id(CallableId{"class_init", 0});

        assert(contains(result.retained, box_id));
        assert(contains(result.retained, init_id));
        assert(has_edge(
            result, main_id, box_id,
            SemanticReachabilityReason::ClassReference));
        assert(has_edge(
            result, box_id, init_id,
            SemanticReachabilityReason::ClassInitializer));
    }

    {
        Program program;
        program.functions.push_back(fn("actor_init", integer(0)));
        program.functions.push_back(fn("actor_helper", integer(1)));

        ActorDecl worker;
        worker.name = "Worker";
        LocalBinding seed;
        seed.name = "seed";
        seed.initializer = call("actor_init");
        worker.fields.push_back(std::move(seed));
        worker.on_message =
            fn("on_message", call("actor_helper"), true);
        program.actors.push_back(std::move(worker));

        Function main = fn("main", integer(0), true);
        ActorSpawnBinding spawn;
        spawn.name = "worker";
        spawn.actor_type = "Worker";
        main.actor_spawns.push_back(std::move(spawn));
        program.functions.push_back(std::move(main));

        const auto result = analyze_semantic_reachability(program);
        const auto main_id =
            function_declaration_id(CallableId{"main", 0});
        const auto actor_id = actor_declaration_id("Worker");
        const auto init_id =
            function_declaration_id(CallableId{"actor_init", 0});
        const auto helper_id =
            function_declaration_id(CallableId{"actor_helper", 0});

        assert(contains(result.retained, actor_id));
        assert(contains(result.retained, init_id));
        assert(contains(result.retained, helper_id));
        assert(has_edge(
            result, main_id, actor_id,
            SemanticReachabilityReason::ActorSpawn));
        assert(has_edge(
            result, actor_id, init_id,
            SemanticReachabilityReason::ActorInitializer));
        assert(has_edge(
            result, actor_id, helper_id,
            SemanticReachabilityReason::ActorHandler));
    }

    {
        Program program;
        Function api_one = fn("api", integer(1), true);
        Parameter one;
        one.name = "value";
        api_one.parameters.push_back(std::move(one));
        program.functions.push_back(std::move(api_one));

        Function api_two = fn("api", integer(2), false);
        Parameter left;
        left.name = "left";
        Parameter right;
        right.name = "right";
        api_two.parameters.push_back(std::move(left));
        api_two.parameters.push_back(std::move(right));
        program.functions.push_back(std::move(api_two));

        SemanticRootPolicy policy;
        policy.build_kind = SemanticBuildKind::Library;
        const auto result =
            analyze_semantic_reachability(program, policy);

        const auto api1 =
            function_declaration_id(CallableId{"api", 1});
        const auto api2 =
            function_declaration_id(CallableId{"api", 2});
        assert(contains(result.roots, api1));
        assert(contains(result.retained, api2));
        assert(has_edge(
            result,
            api1,
            api2,
            SemanticReachabilityReason::ExportedOverloadAbi));
    }

    {
        Program program;
        program.functions.push_back(
            fn("public_api", call("private_helper"), true));
        program.functions.push_back(
            fn("private_helper", integer(8)));
        program.functions.push_back(
            fn("unused", integer(9)));

        SemanticRootPolicy policy;
        policy.build_kind = SemanticBuildKind::Library;
        policy.explicit_function_roots.push_back(
            CallableId{"unused", 0});
        const auto result =
            analyze_semantic_reachability(program, policy);

        const auto api_id =
            function_declaration_id(CallableId{"public_api", 0});
        const auto helper_id =
            function_declaration_id(CallableId{"private_helper", 0});
        const auto unused_id =
            function_declaration_id(CallableId{"unused", 0});

        assert(contains(result.roots, api_id));
        assert(contains(result.roots, unused_id));
        assert(contains(result.retained, helper_id));
        assert(!contains(result.removed, unused_id));

        const auto api =
            explain_semantic_reachability(result, api_id);
        assert(api.root);
        assert(std::find(
            api.root_reasons.begin(),
            api.root_reasons.end(),
            SemanticReachabilityReason::RootPublicApi) !=
            api.root_reasons.end());
    }

    {
        Program program;
        program.functions.push_back(fn("target_helper", integer(5)));

        ActorDecl actor;
        actor.name = "Hosted";
        actor.on_message =
            fn("on_message", call("target_helper"), true);
        program.actors.push_back(std::move(actor));

        SemanticRootPolicy policy;
        policy.root_default_main = false;
        policy.target_roots.push_back(
            actor_declaration_id("Hosted"));
        const auto result =
            analyze_semantic_reachability(program, policy);

        const auto actor_id = actor_declaration_id("Hosted");
        const auto helper_id =
            function_declaration_id(CallableId{"target_helper", 0});
        assert(contains(result.roots, actor_id));
        assert(contains(result.retained, helper_id));

        const auto explanation =
            explain_semantic_reachability(result, actor_id);
        assert(explanation.root);
        assert(std::find(
            explanation.root_reasons.begin(),
            explanation.root_reasons.end(),
            SemanticReachabilityReason::RootTargetAbi) !=
            explanation.root_reasons.end());
    }

    {
        Program program;
        Function continuation =
            fn("step", integer(0), true);
        continuation.scalar_while =
            std::make_unique<ScalarWhileLoop>();
        continuation.scalar_while->counter = "count";
        continuation.scalar_while->upper_bound = 4;
        program.functions.push_back(std::move(continuation));

        CooperativeScalarLoop loop;
        loop.name = "spin";
        program.cooperative_loops.push_back(std::move(loop));

        SemanticRootPolicy policy;
        policy.root_default_main = false;
        const auto result =
            analyze_semantic_reachability(program, policy);

        const auto step_id =
            function_declaration_id(CallableId{"step", 0});
        const auto spin_id =
            continuation_declaration_id("spin");
        assert(contains(result.roots, step_id));
        assert(contains(result.roots, spin_id));
    }

    {
        Function deep = fn("main", nullptr, true);
        auto root = std::make_unique<Expression>();
        root->kind = Expression::Kind::Negate;
        root->left = std::make_unique<Expression>();
        root->left->kind = Expression::Kind::Negate;
        root->left->left = integer(1);
        deep.result = std::move(root);

        SemanticScanBudget budget;
        budget.max_ast_depth = 2;
        bool rejected = false;
        try {
            (void)scan_semantic_dependencies(deep, budget);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        Program program;
        program.functions.push_back(fn("main", call("missing"), true));

        bool rejected = false;
        try {
            (void)analyze_semantic_reachability(program);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "unresolved dependency must fail closed");
    }

    {
        Program program;
        program.functions.push_back(fn("main", call("leaf"), true));
        program.functions.push_back(fn("leaf", integer(1)));

        SemanticGraphBudget budget;
        budget.max_edges = 0;
        bool rejected = false;
        try {
            (void)analyze_semantic_reachability(
                program, SemanticRootPolicy{}, budget);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "semantic graph edge budget must fail closed");
    }

    {
        Program program;
        program.functions.push_back(fn("main", call("leaf"), true));
        program.functions.push_back(fn("leaf", integer(1)));

        SemanticGraphBudget budget;
        budget.max_depth = 0;
        bool rejected = false;
        try {
            (void)analyze_semantic_reachability(
                program, SemanticRootPolicy{}, budget);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "semantic graph depth budget must fail closed");
    }

    {
        Program program;
        program.functions.push_back(fn("main", integer(0), true));

        SemanticGraphBudget budget;
        budget.max_declarations = 0;
        bool rejected = false;
        try {
            (void)analyze_semantic_reachability(
                program, SemanticRootPolicy{}, budget);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "semantic declaration budget must fail closed");
    }

    {
        Program program;
        program.functions.push_back(fn("main", integer(0), true));
        program.functions.push_back(fn("dead", integer(1)));

        const auto result = analyze_semantic_reachability(program);
        bool rejected = false;
        try {
            (void)explain_semantic_reachability(
                result,
                function_declaration_id(CallableId{"dead", 0}));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected && "removed declaration must not be explainable");
    }

    {
        Program program;
        program.functions.push_back(fn("pick", integer(1)));
        Function pick_two = fn("pick", integer(2));
        Parameter first;
        first.name = "first";
        Parameter second;
        second.name = "second";
        pick_two.parameters.push_back(std::move(first));
        pick_two.parameters.push_back(std::move(second));
        program.functions.push_back(std::move(pick_two));
        program.functions.push_back(fn("main", call("pick"), true));

        const auto result = analyze_semantic_reachability(program);
        bool rejected = false;
        try {
            (void)make_reachability_manifest_v1(
                program, result);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected &&
               "v1 manifest must reject undefined shared overload identity");
    }

    {
        Program program;
        program.functions.push_back(fn("Thing", integer(1)));
        program.functions.push_back(fn("main", integer(0), true));
        ClassDecl klass;
        klass.name = "Thing";
        program.classes.push_back(std::move(klass));

        const auto result = analyze_semantic_reachability(program);
        bool rejected = false;
        try {
            (void)make_reachability_manifest_v1(
                program, result);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected &&
               "v1 manifest must reject lexical semantic symbol collisions");
    }

    {
        OresReachabilityManifestV1 forged;
        forged.build_kind = "executable";
        forged.roots = {"__root__.main"};
        forged.retained_symbols = {"__root__.leaf"};
        bool rejected = false;
        try {
            (void)canonical_reachability_manifest_json(forged);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected &&
               "canonical serialization must validate forged root closure");
    }

    {
        Program program;
        program.functions.push_back(fn("main", integer(0), true));
        program.functions.push_back(fn("dead", integer(1)));
        auto result = analyze_semantic_reachability(program);
        result.removed.clear();

        bool rejected = false;
        try {
            (void)make_reachability_manifest_v1(program, result);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected &&
               "manifest adapter must reject incomplete declaration classification");
    }

    {
        Program program;
        program.functions.push_back(fn("main", integer(0), true));
        ClassDecl first;
        first.name = "Duplicate";
        ClassDecl second;
        second.name = "Duplicate";
        program.classes.push_back(std::move(first));
        program.classes.push_back(std::move(second));

        SemanticReachabilityResult forged;
        forged.build_kind = SemanticBuildKind::Executable;
        forged.roots.push_back(
            function_declaration_id(CallableId{"main", 0}));
        forged.retained = forged.roots;
        forged.removed.push_back(class_declaration_id("Duplicate"));

        bool rejected = false;
        try {
            (void)make_reachability_manifest_v1(program, forged);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected &&
               "manifest adapter must reject duplicate program declaration IDs");
    }

    {
        Program program;
        program.functions.push_back(fn("orphan", integer(1)));
        SemanticRootPolicy policy;
        policy.root_default_main = false;

        bool rejected = false;
        try {
            (void)analyze_semantic_reachability(program, policy);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    return 0;
}
