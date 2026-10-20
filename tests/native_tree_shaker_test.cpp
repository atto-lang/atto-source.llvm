#include "oreslang/frontend/TreeShaker.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>

namespace {

std::unique_ptr<oreslang::Expression> integer(std::int64_t value) {
    auto expression = std::make_unique<oreslang::Expression>();
    expression->kind = oreslang::Expression::Kind::Integer;
    expression->integer = value;
    return expression;
}

std::unique_ptr<oreslang::Expression> call(std::string name) {
    auto expression = std::make_unique<oreslang::Expression>();
    expression->kind = oreslang::Expression::Kind::Call;
    expression->name = std::move(name);
    return expression;
}

oreslang::Function function(std::string name, bool exported = false) {
    oreslang::Function fn;
    fn.name = std::move(name);
    fn.exported = exported;
    fn.result = integer(1);
    return fn;
}

} // namespace

int main() {
    {
        oreslang::Program program;
        auto main = function("main");
        main.result = call("live");
        auto live = function("live");
        auto dead = function("dead");

        oreslang::LocalBinding dead_owner;
        dead_owner.name = "owned";
        dead_owner.heap_owned = true;
        dead_owner.initializer = integer(7);
        dead.locals.push_back(std::move(dead_owner));

        program.functions.push_back(std::move(main));
        program.functions.push_back(std::move(live));
        program.functions.push_back(std::move(dead));

        auto manifest = oreslang::tree_shake_program(
            program, oreslang::TreeShakeMode::Executable);

        assert(program.functions.size() == 2);
        assert(manifest.entry_points.size() == 1 && manifest.entry_points.front() == "main");
        assert(manifest.removed_symbols.size() == 1);
        assert(manifest.removed_symbols.front() == "__root__.dead$arity0");
        assert(std::find(manifest.runtime_operations.begin(),
                         manifest.runtime_operations.end(),
                         "native-owned-i64-region") == manifest.runtime_operations.end());
        assert(manifest.to_canonical_json().find(
            "\"schema\":\"oreslang.reachability/1\"") != std::string::npos);
    }

    {
        oreslang::Program program;
        program.functions.push_back(function("public_api", true));
        program.functions.push_back(function("private_dead", false));

        auto manifest = oreslang::tree_shake_program(
            program, oreslang::TreeShakeMode::Library);

        assert(program.functions.size() == 1);
        assert(program.functions.front().name == "public_api");
        assert(manifest.preserve_public_api);
        assert(manifest.entry_points.empty());
        assert(manifest.removed_symbols.front() == "__root__.private_dead$arity0");
    }

    {
        oreslang::Program program;

        auto main = function("main");
        main.result = call("pick");
        main.result->arguments.push_back(integer(9));

        auto pick_one = function("pick");
        pick_one.parameters.push_back(oreslang::Parameter{"value"});
        pick_one.result = call("live_dep");

        auto pick_two = function("pick");
        pick_two.parameters.push_back(oreslang::Parameter{"a"});
        pick_two.parameters.push_back(oreslang::Parameter{"b"});
        pick_two.result = call("dead_dep");

        auto live_dep = function("live_dep");
        auto dead_dep = function("dead_dep");

        program.functions.push_back(std::move(main));
        program.functions.push_back(std::move(pick_one));
        program.functions.push_back(std::move(pick_two));
        program.functions.push_back(std::move(live_dep));
        program.functions.push_back(std::move(dead_dep));

        auto manifest = oreslang::tree_shake_program(
            program, oreslang::TreeShakeMode::Executable);

        assert(program.functions.size() == 3);
        assert(std::any_of(program.functions.begin(), program.functions.end(),
            [](const oreslang::Function& fn) {
                return fn.name == "pick" && fn.parameters.size() == 1;
            }));
        assert(std::none_of(program.functions.begin(), program.functions.end(),
            [](const oreslang::Function& fn) {
                return fn.name == "pick" && fn.parameters.size() == 2;
            }));
        assert(std::find(manifest.removed_symbols.begin(),
                         manifest.removed_symbols.end(),
                         "__root__.pick$arity2") != manifest.removed_symbols.end());
        assert(std::find(manifest.removed_symbols.begin(),
                         manifest.removed_symbols.end(),
                         "__root__.dead_dep$arity0") != manifest.removed_symbols.end());
    }

    return 0;
}
