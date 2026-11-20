#include "oreslang/runtime/NestedActorPipelinePlan.hpp"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace oreslang::runtime;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Fn>
static void must_reject(Fn&& function, const char* expected) {
    bool matched = false;
    try { function(); }
    catch (const std::invalid_argument& error) {
        matched = std::string(error.what()).find(expected) != std::string::npos;
    }
    check(matched, "expected fail-closed nested pipe validation error");
}

static NestedPipePart actor(const char* name, PipeRole role,
                            const char* input, const char* output) {
    return NestedPipePart::actor({name, role, input, output});
}

template <class... Items>
static NestedPipePart pipe_segment(
        std::optional<std::size_t> capacity,
        std::optional<PipeRestartStrategy> policy,
        Items&&... items) {
    std::vector<NestedPipePart> children;
    children.reserve(sizeof...(Items));
    (children.push_back(std::forward<Items>(items)), ...);
    return NestedPipePart::group(std::move(children), capacity, policy);
}

static NestedPipePart nested(
        std::optional<std::size_t> inner_capacity = std::nullopt,
        std::optional<PipeRestartStrategy> inner_policy = std::nullopt,
        std::optional<PipeRestartStrategy> outer_policy = PipeRestartStrategy::one_for_all) {
    return pipe_segment(1024, outer_policy,
        actor("Reader", PipeRole::readable, "", "Bytes"),
        pipe_segment(inner_capacity, inner_policy,
            actor("T1", PipeRole::transform, "Bytes", "Csv"),
            actor("T2", PipeRole::transform, "Csv", "Rows")),
        actor("Writer", PipeRole::writable, "Rows", ""));
}

static_assert(!std::is_copy_constructible<NestedPipePart>::value,
              "unstarted pipe construction descriptors must not be copyable");
static_assert(!std::is_default_constructible<NestedPipePart>::value,
              "unstarted pipe descriptors must be factory-constructed");
static_assert(!std::is_default_constructible<NestedPipePlan>::value,
              "only the checked planner can create a graph plan");
static_assert(std::is_same<decltype(std::declval<NestedPipePlan&>().actors()),
                           const std::vector<PlannedPipeActor>&>::value,
              "actor list must not be mutable after planning");
static_assert(std::is_same<decltype(std::declval<NestedPipePlan&>().scopes()),
                           const std::vector<PlannedPipeScope>&>::value,
              "supervisor selection must not accept edited children");
static_assert(std::is_move_constructible<NestedPipePart>::value,
              "unstarted pipe construction descriptors must allow ownership moves");

int main() {
    must_reject([] {
        (void)check_nested_actor_pipeline(nested(
            std::nullopt, static_cast<PipeRestartStrategy>(255)));
    }, "invalid nested actor pipe supervisor policy");
    {
        const auto graph = check_nested_actor_pipeline(nested());
        check(graph.actors().size() == 4 && graph.edges().size() == 3,
              "nested graph did not flatten 4 nodes/3 DATA edges");
        check(graph.actors()[0].stage.actor_type == "Reader" &&
                graph.actors()[3].stage.actor_type == "Writer", "stage order changed");
        check(graph.edges()[0].payload == "Bytes" &&
                graph.edges()[1].payload == "Csv" &&
                graph.edges()[2].payload == "Rows", "edge type order changed");
        for (std::size_t index = 0; index < graph.edges().size(); ++index) {
            const auto& edge = graph.edges()[index];
            check(edge.source_index == index && edge.target_index == index + 1,
                  "nested pipe edge indices do not match source order");
            check(edge.source_path == graph.actors()[index].path &&
                    edge.target_path == graph.actors()[index + 1].path,
                  "nested pipe path no longer matches checked stage indices");
        }
        check(graph.edges()[0].capacity == 1024 &&
                graph.edges()[1].capacity == 1024 &&
                graph.edges()[2].capacity == 1024 &&
                graph.total_slots() == 3072, "capacity inheritance failed");
        check(graph.scopes().size() == 1 && graph.scopes()[0].physical_supervisor,
              "outer explicit supervision must produce one physical scope");
        check(graph.scopes()[0].children.size() == 4 &&
                !graph.scopes()[0].children[1].is_supervisor, "plain nested segment reparented actors");
        check(graph.actors()[1].owner_scope == "root", "unsupervised inner scope minted owner");
    }
    {
        const auto graph = check_nested_actor_pipeline(
            pipe_segment(1024, PipeRestartStrategy::one_for_all,
                actor("Reader", PipeRole::readable, "", "Bytes"),
                actor("T1", PipeRole::transform, "Bytes", "Csv"),
                actor("T2", PipeRole::transform, "Csv", "Rows"),
                actor("Writer", PipeRole::writable, "Rows", "")));
        check(graph.actors().size() == 4 && graph.edges().size() == 3,
              "flat chain not equivalent to nested");
        check(graph.total_slots() == 3072, "flat capacities not equivalent");
    }
    {
        const auto graph = check_nested_actor_pipeline(
            nested(16, PipeRestartStrategy::one_for_one));
        check(graph.scopes().size() == 2 && graph.scopes()[1].parent_path == "root" &&
                graph.scopes()[1].path == "root/1", "nested supervisor parent wrong");
        check(graph.scopes()[0].children.size() == 3 &&
                graph.scopes()[0].children[1].is_supervisor, "nested scope not one child");
        check(graph.actors()[1].owner_scope == "root/1" &&
                graph.actors()[2].owner_scope == "root/1", "inner actors wrong supervisor");
        check(graph.edges()[0].capacity == 1024 && graph.edges()[1].capacity == 16 &&
                graph.edges()[2].capacity == 1024 && graph.total_slots() == 2064,
              "nested internal edge capacity leaked to boundaries");
        check(graph.edges()[0].owner_scope == "root" &&
                graph.edges()[1].owner_scope == "root/1" &&
                graph.edges()[2].owner_scope == "root", "wrong cross-boundary edge owner");
        auto restart = nested_pipe_restart_targets(graph, "root", "root/0");
        check(restart == std::vector<std::string>({"root/0","root/1","root/2"}),
              "one_for_all restart did not treat nested supervisor as one child");
        restart = nested_pipe_restart_targets(graph, "root/1", "root/1/0");
        check(restart == std::vector<std::string>({"root/1/0"}),
              "one_for_one grandchild restart escaped its supervisor");
        must_reject([&] {
            (void)nested_pipe_restart_targets(graph, "root", "root/1/0");
        }, "not a direct");
    }
    {
        const auto graph = check_nested_actor_pipeline(
            nested(std::nullopt, PipeRestartStrategy::one_for_one,
                   PipeRestartStrategy::rest_for_one));
        const auto affected = nested_pipe_restart_targets(graph, "root", "root/1");
        check(affected == std::vector<std::string>({"root/1","root/2"}),
              "rest_for_one must restart nested scope and later direct siblings only");
    }
    {
        const auto graph = check_nested_actor_pipeline(nested(std::nullopt,std::nullopt,std::nullopt));
        check(graph.scopes().size() == 1 && !graph.scopes()[0].physical_supervisor,
              "unsupervised graph must inherit caller supervisor");
        must_reject([&] {
            (void)nested_pipe_restart_targets(graph, "root", "root/0");
        }, "no explicit supervisor");
    }
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt, std::nullopt,
            actor("Single", PipeRole::readable, "", "Bytes")));
    }, "at least two stages");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt,std::nullopt,
            actor("Reader", PipeRole::readable,"","Bytes"),
            pipe_segment(std::nullopt,std::nullopt),
            actor("Writer",PipeRole::writable,"Bytes","")));
    }, "empty nested");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt,std::nullopt,
            actor("Reader",PipeRole::readable,"","Bytes"),
            pipe_segment(std::nullopt,std::nullopt,
                actor("T",PipeRole::transform,"Wrong","Csv")),
            actor("Writer",PipeRole::writable,"Csv","")));
    }, "payload mismatch");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt,std::nullopt,
            actor("Reader",PipeRole::readable,"","Bytes"),
            actor("Supervisor",PipeRole::supervisor_actor,"Bytes","Csv"),
            actor("Writer",PipeRole::writable,"Csv","")));
    }, "supervisor actors");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(0,std::nullopt,
            actor("Reader",PipeRole::readable,"","Bytes"),
            actor("Writer",PipeRole::writable,"Bytes","")));
    }, "capacity");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(65537,std::nullopt,
            actor("Reader",PipeRole::readable,"","Bytes"),
            actor("Writer",PipeRole::writable,"Bytes","")));
    }, "capacity");
    {
        auto limits = NestedPipeLimits{};
        limits.max_total_slots = 2063;
        must_reject([&] {
            (void)check_nested_actor_pipeline(
                nested(16, PipeRestartStrategy::one_for_one), limits);
        }, "aggregate slot");
        limits.max_total_slots = 2064;
        check(check_nested_actor_pipeline(
            nested(16, PipeRestartStrategy::one_for_one), limits).total_slots() == 2064,
            "exact slot quota should admit");
        limits.max_stages = 3;
        must_reject([&] { (void)check_nested_actor_pipeline(nested(),limits); }, "stage budget");
    }

    must_reject([] {
        auto limits = NestedPipeLimits{};
        limits.max_segments = 1;
        (void)check_nested_actor_pipeline(nested(), limits);
    }, "segment budget");
    must_reject([] {
        auto limits = NestedPipeLimits{};
        limits.max_segments = 2049;
        (void)check_nested_actor_pipeline(nested(), limits);
    }, "resource limits");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt, std::nullopt,
            NestedPipePart::actor({std::string(257, 'A'), PipeRole::readable, "", "Bytes"}),
            actor("Writer", PipeRole::writable, "Bytes", "")));
    }, "metadata byte budget");
    must_reject([] {
        (void)check_nested_actor_pipeline(pipe_segment(std::nullopt, std::nullopt,
            actor("Reader", PipeRole::readable, "", "Bytes"),
            NestedPipePart::actor({"Writer", PipeRole::writable,
                std::string("By") + char(0) + "tes", ""})));
    }, "metadata control byte");
    {
        auto limits = NestedPipeLimits{};
        limits.max_edges = 2;
        must_reject([&] { (void)check_nested_actor_pipeline(nested(),limits); }, "edge budget");
        limits = {};
        limits.max_scopes = 1;
        must_reject([&] {
            (void)check_nested_actor_pipeline(
                nested(16,PipeRestartStrategy::one_for_one),limits);
        }, "scope budget");
        limits = {};
        limits.max_depth = 1;
        must_reject([&] { (void)check_nested_actor_pipeline(nested(),limits); }, "depth budget");
        limits = {};
        limits.max_depth = std::numeric_limits<std::size_t>::max();
        must_reject([&] { (void)check_nested_actor_pipeline(nested(),limits); }, "resource limits");
    }
}
