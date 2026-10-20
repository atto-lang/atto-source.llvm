#include "oreslang/frontend/Reachability.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;

static std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expression = std::make_unique<Expression>();
    expression->kind = Expression::Kind::Integer;
    expression->integer = value;
    return expression;
}

static std::unique_ptr<Expression> call(
        const std::string& name,
        std::vector<std::unique_ptr<Expression>> arguments = {}) {
    auto expression = std::make_unique<Expression>();
    expression->kind = Expression::Kind::Call;
    expression->name = name;
    expression->arguments = std::move(arguments);
    return expression;
}

static Function function(
        const std::string& name,
        std::unique_ptr<Expression> result,
        bool exported = false) {
    Function output;
    output.name = name;
    output.result = std::move(result);
    output.exported = exported;
    return output;
}

int main() {
    {
        Program program;
        Function dead = function("dead_array", integer(0));
        LocalBinding array;
        array.name = "items";
        array.int_array = true;
        dead.locals.push_back(std::move(array));
        program.functions.push_back(std::move(dead));
        program.functions.push_back(function("main", integer(42), true));

        const auto before = analyze_runtime_features(program, false, false);
        assert(before.contains(RuntimeFeature::IntArrayRuntime));

        auto shaken = tree_shake_program(std::move(program), {"main"});
        const auto after = analyze_runtime_features(shaken.program, false, false);
        assert(after.names().empty());
        assert(shaken.program.functions.size() == 1);
        assert(shaken.program.functions.front().name == "main");
    }

    {
        Program program;
        Function main = function("main", integer(42), true);
        LocalBinding owned;
        owned.name = "value";
        owned.heap_owned = true;
        owned.initializer = integer(42);
        main.locals.push_back(std::move(owned));
        program.functions.push_back(std::move(main));

        const auto features = analyze_runtime_features(program, false, false);
        assert(features.names() ==
               std::vector<std::string>{"owned-i64-region"});
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";
        actor.on_message = function("on_message", integer(42), true);
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
        assert(rejected);
    }

    {
        Program program;

        Function one = function("pick", integer(1));
        Parameter one_parameter;
        one_parameter.name = "x";
        one.parameters.push_back(std::move(one_parameter));
        program.functions.push_back(std::move(one));

        Function two = function("pick", integer(2));
        Parameter left;
        left.name = "x";
        Parameter right;
        right.name = "y";
        two.parameters.push_back(std::move(left));
        two.parameters.push_back(std::move(right));
        program.functions.push_back(std::move(two));

        std::vector<std::unique_ptr<Expression>> arguments;
        arguments.push_back(integer(40));
        arguments.push_back(integer(2));
        program.functions.push_back(
                function("main", call("pick", std::move(arguments)), true));

        auto shaken = tree_shake_program(std::move(program), {});
        bool saw_one = false;
        bool saw_two = false;
        for (const auto& fn : shaken.program.functions) {
            if (fn.name != "pick") continue;
            if (fn.parameters.size() == 1) saw_one = true;
            if (fn.parameters.size() == 2) saw_two = true;
        }
        assert(!saw_one);
        assert(saw_two);
    }

    return 0;
}
