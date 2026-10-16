#include "oreslang/runtime/NativeActorPipeTransport.hpp"
#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

int main() {
    NativeSupervisorRegistry registry(25, 25, 25);
    auto supervisor = registry.create_child(registry.root());
    std::vector<PipeStage> stages{
        {"Producer", PipeRole::readable, "", "Event"},
        {"Consumer", PipeRole::writable, "Event", ""}
    };
    for (int trial = 0; trial < 32; ++trial) {
        NativeActorPipeTransport pipe(registry, supervisor, stages, 1, 4);
        pipe.write_one_async("Producer", "front").get();
        auto pending = pipe.write_one_async("Producer", "back");
        require(pending.wait_for(0ms) == std::future_status::timeout,
                "second write was not queued");
        require(pipe.buffered_payload_bytes() == 9
                && pipe.buffered_message_count() == 2,
                "pending writer budget was not charged");
#ifdef ORES_ACTOR_PIPE_TEST_FAULT_INJECTION
        pipe.test_fail_next_writer_promotion();
#endif
        require(pipe.read_async(0).get() == "front",
                "reader result changed by writer promotion");
#ifdef ORES_ACTOR_PIPE_TEST_FAULT_INJECTION
        bool failed = false;
        try { pending.get(); }
        catch (const std::bad_alloc&) { failed = true; }
        require(failed, "fault-injected writer Future was not rejected");
#else
        pending.get();
        require(pipe.read_async(0).get() == "back",
                "successfully promoted writer was not delivered");
#endif
        require(pipe.buffered_payload_bytes() == 0
                && pipe.buffered_message_count() == 0,
                "writer promotion leaked global graph credits");
        pipe.write_one_async("Producer", "recovered").get();
        require(pipe.read_async(0).get() == "recovered",
                "transport failed to recover after writer promotion");
        pipe.close();
    }
}
