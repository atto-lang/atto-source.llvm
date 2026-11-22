#include "oreslang/runtime/ActorPipeContract.hpp"
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::runtime;

static void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
static void rejects(const std::vector<PipeStage>& stages, const std::string& diagnostic) {
    bool rejected = false;
    try { (void)check_actor_pipeline(stages); }
    catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find(diagnostic) != std::string::npos;
    }
    check(rejected, "missing expected compile-time actor pipe diagnostic");
}
int main() {
    const PipeStage source{"A1", PipeRole::readable, "", "Event"};
    const PipeStage middle{"A2", PipeRole::transform, "Event", "Result"};
    const PipeStage sink{"A3", PipeRole::writable, "Result", ""};
    const auto links = check_actor_pipeline({source, middle, sink});
    check(links.size() == 2 && links[0].payload == "Event"
            && links[1].payload == "Result", "typed pipe admission broken");
    for (std::size_t i = 0; i < 3; ++i) {
        auto stages = std::vector<PipeStage>{source, middle, sink};
        stages[i] = PipeStage{"Daddy", PipeRole::supervisor_actor, "Event", "Result"};
        rejects(stages, "supervisor actors");
        stages[i] = PipeStage{"Ordinary", PipeRole::ordinary_actor, "Event", "Result"};
        rejects(stages, "ordinary actor");
    }
    rejects({sink, sink}, "source");
    rejects({source, source, sink}, "middle");
    rejects({source, {"T", PipeRole::transform, "string", "string"},
             {"W", PipeRole::writable, "string", ""}}, "payload mismatch");
    rejects({source}, "two stages");
    rejects({{"Bad", PipeRole::readable, "Event", "Result"}, sink}, "only emit");
    rejects({{"Bad", PipeRole::readable, "", ""}, sink}, "only emit");
    rejects({{"Bad", PipeRole::duplex, "", "Result"}, sink}, "input and output");
    // Distinct physical actor instances may share one nominal Worker type.
    // Linear source->worker-a->worker-b->sink must route by instance ID.
    const auto duplicates = check_actor_pipeline({
        {"Producer", PipeRole::readable, "", "Event", "source-1"},
        {"Worker", PipeRole::transform, "Event", "Event", "worker-a"},
        {"Worker", PipeRole::transform, "Event", "Event", "worker-b"},
        {"Sink", PipeRole::writable, "Event", "", "sink-1"}
    });
    check(duplicates.size() == 3
            && duplicates[0].source == "source-1"
            && duplicates[0].target == "worker-a"
            && duplicates[1].source == "worker-a"
            && duplicates[1].target == "worker-b"
            && duplicates[2].source == "worker-b"
            && duplicates[2].target == "sink-1",
            "linear pipeline collapsed same-type worker instances");
    rejects({{"Producer", PipeRole::readable, "", "Event", "same"},
             {"Consumer", PipeRole::writable, "Event", "", "same"}},
            "unique bounded actor instance IDs");
    rejects({{"Producer", PipeRole::readable, "", "Event"},
             {"Producer", PipeRole::writable, "Event", ""}},
            "unique bounded actor instance IDs");
    auto d = PipeStage{"Duplex", PipeRole::duplex, "Event", "Result"};
    check(check_actor_pipeline({source, d, sink}).size() == 2, "duplex stage lost");
}
