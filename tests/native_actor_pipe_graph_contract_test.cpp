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
