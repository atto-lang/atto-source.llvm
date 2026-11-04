#include "oreslang/types/NativeGuestCallGraphGate.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Gate = oreslang::NativeGuestCallGraphGate;

void rejects(const Gate::Graph& graph, const std::string& expected) {
    bool observed = false;
    try {
        (void)Gate::require_bounded(graph);
    } catch (const std::runtime_error& error) {
        observed = std::string(error.what()).find(expected) != std::string::npos;
    }
    assert(observed && "unsafe native guest call graph was admitted");
}

std::unique_ptr<oreslang::Expression> call(
        const std::string& name, std::size_t arity = 0) {
    auto result = std::make_unique<oreslang::Expression>();
    result->kind = oreslang::Expression::Kind::Call;
    result->name = name;
    for (std::size_t n = 0; n < arity; ++n) {
        auto arg = std::make_unique<oreslang::Expression>();
        arg->kind = oreslang::Expression::Kind::Integer;
        result->arguments.push_back(std::move(arg));
    }
    return result;
}
} // namespace

int main() {
    // Empty graph and bounded call chains are safe to admit.
    assert(Gate::require_bounded({}) == 0);
    assert(Gate::require_bounded({{"main/0", {"helper/1"}},
                                  {"helper/1", {}}}) == 2);
    // The scanner follows nested conditionals and constructor fields.
    oreslang::Program nested;
    oreslang::Function fn;
    fn.name = "main";
    fn.result = std::make_unique<oreslang::Expression>();
    fn.result->kind = oreslang::Expression::Kind::Conditional;
    fn.result->when_true = call("helper", 1);
    fn.result->when_false = call("helper", 1);
    nested.functions.push_back(std::move(fn));
    oreslang::Function helper;
    helper.name = "helper";
    helper.parameters.emplace_back();
    nested.functions.push_back(std::move(helper));
    oreslang::require_native_guest_call_graph_bounded(nested);

    rejects({{"f/0", {"f/0"}}}, "recursive");
    rejects({{"a/0", {"b/0"}}, {"b/0", {"a/0"}}}, "recursive");
    rejects({{"main/0", {}}, {"unreachable/0", {"unreachable/0"}}}, "recursive");
    rejects({{"f/0", {"missing/0"}}}, "unresolved");
    rejects({{"", {}}}, "invalid");
    rejects({{"main/0", {"main/1"}}, {"main/1", {"main/1"}}}, "recursive");

    Gate::Graph at_limit;
    for (std::size_t n = 0; n < Gate::max_depth; ++n) {
        const auto key = "fn" + std::to_string(n) + "/0";
        const auto next = "fn" + std::to_string(n + 1) + "/0";
        at_limit.emplace(key, n + 1 == Gate::max_depth
            ? std::vector<std::string>{} : std::vector<std::string>{next});
    }
    assert(Gate::require_bounded(at_limit) == Gate::max_depth);
    at_limit["fn63/0"].push_back("fn64/0");
    at_limit.emplace("fn64/0", std::vector<std::string>{});
    rejects(at_limit, "depth");

    Gate::Graph too_many;
    for (std::size_t n = 0; n <= Gate::max_functions; ++n)
        too_many.emplace("fn" + std::to_string(n), std::vector<std::string>{});
    rejects(too_many, "function budget");

    Gate::Graph too_many_edges;
    too_many_edges.emplace("main/0",
        std::vector<std::string>(Gate::max_edges + 1, "main/0"));
    rejects(too_many_edges, "edge budget");

    // Actor handler direct calls must be graph-scanned even though handlers
    // are not entries in Program::functions.
    oreslang::Program actor_graph;
    oreslang::ActorDecl actor;
    actor.on_message.result = call("recursive", 1);
    actor_graph.actors.push_back(std::move(actor));
    oreslang::Function recursive;
    recursive.name = "recursive";
    recursive.parameters.emplace_back();
    recursive.result = call("recursive", 1);
    actor_graph.functions.push_back(std::move(recursive));
    bool actor_denied = false;
    try {
        oreslang::require_native_guest_call_graph_bounded(actor_graph);
    } catch (const std::runtime_error& error) {
        actor_denied = std::string(error.what()).find("recursive") != std::string::npos;
    }
    assert(actor_denied);

    // A malformed duplicate overload slot is never trusted as a unique node.
    oreslang::Program duplicate;
    oreslang::Function first; first.name = "same";
    oreslang::Function second; second.name = "same";
    duplicate.functions.push_back(std::move(first));
    duplicate.functions.push_back(std::move(second));
    bool duplicate_denied = false;
    try { oreslang::require_native_guest_call_graph_bounded(duplicate); }
    catch (const std::runtime_error& error) {
        duplicate_denied = std::string(error.what()).find("collision") != std::string::npos;
    }
    assert(duplicate_denied);
    return 0;
}
