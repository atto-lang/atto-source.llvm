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
    // Strict graph-string validation must match the Java planner.
    deny([&] { auto wrong=stages;wrong[0].instance_id=std::string(1, char(0));
               (void)check_actor_pipeline_graph(wrong, links); });
    deny([&] { auto wrong=stages;wrong[0].instance_id=std::string(257, 'z');
               (void)check_actor_pipeline_graph(wrong, links); });
    deny([&] { auto wrong=stages;wrong[0].outbound=std::string(257, 'x');
               (void)check_actor_pipeline_graph(wrong, links); });
    deny([&] { auto wrong=links;wrong[0].payload=std::string(1, char(0));
               (void)check_actor_pipeline_graph(stages, wrong); });
    deny([&] { auto wrong=stages;wrong[0].instance_id=std::string(1, char(0x80));
               (void)check_actor_pipeline_graph(wrong, links); });
    deny([&] { auto wrong=stages;wrong[0].instance_id=std::string(1, char(0xc0));
               (void)check_actor_pipeline_graph(wrong, links); });
    {
        auto unicode=stages;
        unicode[0].instance_id=std::string({char(0xc3),char(0xa9)});
        auto edges=links;
        for (auto& link: edges)
            if (link.source == "Source") link.source=unicode[0].instance_id;
        expect(check_actor_pipeline_graph(unicode, edges).size() == 4);
    }
    // Strict Unicode-scalar boundary corpus, also exercised by the
    // independent Python UTF-8 oracle. Reject malformed encodings in BOTH
    // graph admission and legacy linear pipe admission.
    {
        const std::vector<PipeStage> linear = {
            {"Reader", PipeRole::readable, "", "Payload"},
            {"Writer", PipeRole::writable, "Payload", ""}
        };
        const std::vector<std::string> invalid = {
            std::string(1, char(0)),
            std::string("\xc0\xaf", 2),           // overlong ASCII
            std::string("\xe0\x80\x80", 3),   // overlong U+0000
            std::string("\xed\xa0\x80", 3),   // surrogate U+D800
            std::string("\xf4\x90\x80\x80", 4), // above U+10FFFF
            std::string("\xe2\x82", 2),          // truncated three-byte
            std::string("\xf0\x9f\x8c", 3),   // truncated four-byte
            std::string(257, 'x')               // oversized byte component
        };
        // Guard the fixture itself: accidentally doubled hex escapes turn
        // invalid octets into printable ASCII and make rejection vacuous.
        expect(invalid.size() == 8 &&
               invalid[1].size() == 2 &&
               static_cast<unsigned char>(invalid[1][0]) == 0xc0 &&
               static_cast<unsigned char>(invalid[1][1]) == 0xaf &&
               invalid[3].size() == 3 &&
               static_cast<unsigned char>(invalid[3][0]) == 0xed &&
               invalid[4].size() == 4 &&
               static_cast<unsigned char>(invalid[4][0]) == 0xf4 &&
               invalid[6].size() == 3 &&
               static_cast<unsigned char>(invalid[6][0]) == 0xf0);
        for (const auto& component : invalid) {
            deny([&] { auto bad=stages;
                       bad[1].actor_type=component;
                       (void)check_actor_pipeline_graph(bad,links); });
            deny([&] { auto bad=linear;
                       bad[1].actor_type=component;
                       (void)check_actor_pipeline(bad); });
            deny([&] { auto bad=links;
                       bad[0].payload=component;
                       (void)check_actor_pipeline_graph(stages,bad); });
        }
        // Exactly 256 bytes is legal; the bound is bytes, not codepoints.
        auto large=stages;
        large[0].instance_id=std::string(256, 'a');
        auto large_links=links;
        for (auto& edge : large_links)
            if (edge.source == "Source") edge.source=large[0].instance_id;
        expect(check_actor_pipeline_graph(large, large_links).size()==4);
        large[0].instance_id.push_back('a');
        deny([&] { (void)check_actor_pipeline_graph(large,large_links); });
    }

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
