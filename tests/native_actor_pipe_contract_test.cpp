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
    // Exercise the canonical keyed selector used by BOTH the native
    // transport and correlation ledger, never a duplicated FNV test helper.
    check(actor_pipe_key_slot("customer-7",3) == 0, "customer affinity drift");
    check(actor_pipe_key_slot("alpha",3) == 0, "alpha affinity drift");
    check(actor_pipe_key_slot("beta",3) == 2, "beta affinity drift");
    check(actor_pipe_key_slot("\xf0\x9f\x8c\x8d",3) == 0,
          "UTF-8 emoji affinity drift");
    check(actor_pipe_key_slot("",3) == 2, "empty key affinity drift");
    check(actor_pipe_key_slot(std::string(1,'\0'),3) == 0,
          "embedded NUL key must be hashed as a full byte");
    check(actor_pipe_key_slot(std::string(4096,'a'),3) < 3,
          "maximum-size UTF-8 key rejected");
    const std::vector<std::string> invalid_keys = {
        std::string(4097,'a'), "\x80", "\xc0\xaf", "\xc1\xbf",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xff", "\xe0\x80\x80",
        "\xe2\x82", "\xf0\x9f\x8c", "\xc2",
        std::string({char(0xe0),char(0xa0),char(0x7f)})
    };
    for (const auto& key : invalid_keys) {
        bool denied = false;
        try { (void)actor_pipe_key_slot(key,3); }
        catch (const std::invalid_argument&) { denied = true; }
        check(denied, "malformed/oversize UTF-8 key incorrectly admitted");
    }
    for (std::size_t count : {std::size_t{0},std::size_t{4097}}) {
        bool denied = false;
        try { (void)actor_pipe_key_slot("alpha", count); }
        catch (const std::invalid_argument&) { denied = true; }
        check(denied, "invalid downstream count accepted");
    }
    // All 0..255 single-byte keys: ASCII (including NUL) is legal;
    // every isolated non-ASCII byte is malformed UTF-8.
    for (int value=0; value<256; ++value) {
        std::string key(1,static_cast<char>(value));
        check(valid_pipe_route_key(key) == (value < 128),
              "single-byte routing key UTF-8 classification drift");
    }
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
