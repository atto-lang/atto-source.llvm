#include "oreslang/frontend/SemanticReachability.hpp"

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
