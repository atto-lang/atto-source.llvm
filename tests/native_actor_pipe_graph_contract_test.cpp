#include "oreslang/runtime/ActorPipeContract.hpp"
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::runtime;

static void expect(bool ok) {
    if (!ok) throw std::runtime_error("native graph contract failure");
}
template<class F> static void deny(F run) {
    bool seen = false;
    try { run(); } catch (const std::invalid_argument&) { seen = true; }
    expect(seen);
}
int main() {
    std::vector<PipeStage> stages = {
        {"Source", PipeRole::readable, "", "Payload"},
        {"Left", PipeRole::transform, "Payload", "Payload"},
        {"Right", PipeRole::transform, "Payload", "Payload"},
        {"Sink", PipeRole::writable, "Payload", ""},
    };
    std::vector<PipeConnection> links = {
        {"Source","Left","Payload"}, {"Source","Right","Payload"},
        {"Left","Sink","Payload"}, {"Right","Sink","Payload"}
    };
    expect(check_actor_pipeline_graph(stages, links).size() == 4);

    // The linear constructor must use instance IDs just like the DAG form.
    std::vector<PipeStage> linear_instances = {
        {"Producer", PipeRole::readable, "", "Payload", "producer-1"},
        {"Worker", PipeRole::transform, "Payload", "Payload", "worker-1"},
        {"Worker", PipeRole::writable, "Payload", "", "worker-2"}
    };
    const auto linear_edges = check_actor_pipeline(linear_instances);
    expect(linear_edges.size() == 2 &&
           linear_edges[0].source == "producer-1" &&
           linear_edges[0].target == "worker-1" &&
           linear_edges[1].source == "worker-1" &&
           linear_edges[1].target == "worker-2");
    deny([&] { auto bad = linear_instances;
               bad[2].instance_id = "worker-1";
               (void)check_actor_pipeline(bad); });
    deny([&] { auto bad = linear_instances;
               bad[1].instance_id.clear();
               bad[2].instance_id.clear();
               (void)check_actor_pipeline(bad); });
    deny([&] { auto bad = linear_instances;
               bad[1].outbound = std::string(257, 'X');
               (void)check_actor_pipeline(bad); });
    deny([&] { auto bad = linear_instances;
               bad[1].instance_id = std::string("worker") + char(0) + "evil";
               (void)check_actor_pipeline(bad); });
    deny([&] { auto bad = linear_instances;
               bad.resize(1025, bad[1]);
               (void)check_actor_pipeline(bad); });

    // Host-facing names and type tags must not contain embedded NUL bytes
    // or exceed their declared byte budgets in either constructor.
    deny([&] { auto bad = stages;
               bad[0].actor_type = std::string("Source") + char(0) + "forged";
               (void)check_actor_pipeline_graph(bad, links); });
    deny([&] { auto bad = stages;
               bad[1].inbound = std::string(257, 'X');
               (void)check_actor_pipeline_graph(bad, links); });
    deny([&] { auto bad = links;
               bad[0].target = std::string(257, 'X');
               (void)check_actor_pipeline_graph(stages, bad); });
    deny([&] { auto bad = links;
               bad[0].payload = std::string("Payload") + char(0) + "other";
               (void)check_actor_pipeline_graph(stages, bad); });


    // Two worker *instances* of one nominal actor type are legal. Graph
    // endpoints resolve instance IDs, not nominal class names.
    std::vector<PipeStage> identical_workers = {
        {"Producer", PipeRole::readable, "", "Payload", "source-1"},
        {"Worker", PipeRole::writable, "Payload", "", "worker-a"},
        {"Worker", PipeRole::writable, "Payload", "", "worker-b"}
    };
    std::vector<PipeConnection> worker_edges = {
        {"source-1", "worker-a", "Payload"},
        {"source-1", "worker-b", "Payload"}
    };
    expect(check_actor_pipeline_graph(identical_workers, worker_edges).size() == 2);
    deny([&] { auto wrong = identical_workers;
               wrong[2].instance_id = "worker-a";
               (void)check_actor_pipeline_graph(wrong, worker_edges); });
    deny([&] { auto wrong = identical_workers;
               wrong[2].actor_type = "";
               (void)check_actor_pipeline_graph(wrong, worker_edges); });
    deny([&] { auto wrong = identical_workers;
               wrong[2].instance_id = std::string(257, 'x');
               (void)check_actor_pipeline_graph(wrong, worker_edges); });
    deny([&] { auto wrong=links;wrong[0].payload="Other";
               (void)check_actor_pipeline_graph(stages,wrong); });
    deny([&] { auto wrong=links;wrong.push_back(wrong[0]);
               (void)check_actor_pipeline_graph(stages,wrong); });
    // Right -> Left by itself is acyclic (both branches only join Sink).
    // Close an actual two-node cycle with Left -> Right -> Left.
    deny([&] { auto wrong=links;
               wrong.push_back({"Left","Right","Payload"});
               wrong.push_back({"Right","Left","Payload"});
               (void)check_actor_pipeline_graph(stages,wrong); });
    deny([&] { auto wrong=links;wrong[0].target="Missing";
               (void)check_actor_pipeline_graph(stages,wrong); });
    deny([&] { auto wrong=stages;wrong[2].actor_type="Left";
               (void)check_actor_pipeline_graph(wrong,links); });
    deny([&] { auto wrong=stages;wrong[2].role=PipeRole::supervisor_actor;
               (void)check_actor_pipeline_graph(wrong,links); });
    deny([&] { auto wrong=stages;wrong.pop_back();
               (void)check_actor_pipeline_graph(wrong,links); });
    // Budget checks operate before graph allocation and fail closed.
    deny([&] { auto wrong=links;wrong.clear();
               (void)check_actor_pipeline_graph(stages,wrong); });
    deny([&] { auto wrong=stages;wrong.resize(1025,stages[1]);
               (void)check_actor_pipeline_graph(wrong,links); });
}
