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
    NativeSupervisorRegistry registry(25, 128, 128);
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
#ifdef ORES_ACTOR_PIPE_TEST_FAULT_INJECTION
    // When a promotion fails, the buffer can have a vacant slot while
    // older writers still own charged pending admissions. A later write
    // must not overtake them. A second failure can also leave NO buffered
    // messages but one live writer: the next read must drain it directly.
    for (int scenario = 0; scenario < 2; ++scenario) {
        NativeActorPipeTransport pipe(registry, supervisor, stages, 2, 4);
        pipe.write_one_async("Producer", "A").get();
        pipe.write_one_async("Producer", "B").get();
        auto first_failed = pipe.write_one_async("Producer", "X");
        auto older = pipe.write_one_async("Producer", "OLD");
        require(first_failed.wait_for(0ms) == std::future_status::timeout
                && older.wait_for(0ms) == std::future_status::timeout,
                "both older writers must start queued");

        pipe.test_fail_next_writer_promotion();
        require(pipe.read_async(0).get() == "A", "first buffered DATA changed");
        bool first_rejected = false;
        try { first_failed.get(); }
        catch (const std::bad_alloc&) { first_rejected = true; }
        require(first_rejected, "first injected promotion fault not reported");
        require(pipe.buffered_payload_bytes() == 4
                && pipe.buffered_message_count() == 2,
                "first injected failure did not preserve remaining credits");

        auto newer = pipe.write_one_async("Producer", "NEW");
        require(newer.wait_for(0ms) == std::future_status::timeout,
                "new writer overtook an older queued writer after promotion fault");
        require(pipe.buffered_payload_bytes() == 7
                && pipe.buffered_message_count() == 3,
                "new pending writer did not charge credits");
        // The buffer has a free slot, but both pending-writer slots are
        // occupied. A third pending write must not bypass the edge quota.
        bool rejected_overflow = false;
        try { pipe.write_one_async("Producer", "EXTRA").get(); }
        catch (const std::logic_error&) { rejected_overflow = true; }
        require(rejected_overflow
                && pipe.buffered_payload_bytes() == 7
                && pipe.buffered_message_count() == 3,
                "pending-writer edge quota bypassed after promotion failure");

        if (scenario == 1)
            pipe.test_fail_next_writer_promotion();
        require(pipe.read_async(0).get() == "B", "second buffered DATA changed");

        if (scenario == 1) {
            bool older_rejected = false;
            try { older.get(); }
            catch (const std::bad_alloc&) { older_rejected = true; }
            require(older_rejected, "second injected promotion fault not reported");
            require(pipe.buffered_payload_bytes() == 3
                    && pipe.buffered_message_count() == 1,
                    "second injected failure leaked or erased newer writer credits");
            // There is now no buffered DATA, but NEW is still queued.
            require(pipe.read_async(0).get() == "NEW",
                    "queued writer became stranded behind an empty mailbox");
            newer.get();
        } else {
            older.get();
            require(pipe.read_async(0).get() == "OLD",
                    "pending writer lost FIFO order after the first fault");
            newer.get();
            require(pipe.read_async(0).get() == "NEW",
                    "younger pending writer was not delivered last");
        }
        require(pipe.buffered_payload_bytes() == 0
                && pipe.buffered_message_count() == 0
                && pipe.pending_reader_count() == 0,
                "recovery left global credits charged");
        pipe.write_one_async("Producer", "healthy").get();
        require(pipe.read_async(0).get() == "healthy",
                "transport cannot accept DATA after repeated promotion failures");
        pipe.close();
    }
#endif
}
