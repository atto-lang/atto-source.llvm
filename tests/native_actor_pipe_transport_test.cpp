#include "oreslang/runtime/NativeActorPipeTransport.hpp"

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}

static std::vector<PipeStage> stages() {
    return {{"A1", PipeRole::readable, "", "Message"},
            {"A2", PipeRole::transform, "Message", "Message"},
            {"A3", PipeRole::writable, "Message", ""}};
}

int main() {
    NativeSupervisorRegistry registry(10, 10, 10);
    auto supervisor = registry.create_child(registry.root());
    {
        NativeActorPipeTransport pipe(registry, supervisor, stages(), 1, 3);
        check(pipe.edge_count() == 2, "missing native pipe edges");
        check(pipe.supervisor_id() == supervisor.id(), "pipe not supervised before publication");
        check(registry.direct_group_ids(supervisor).size() == 1, "unregistered pipe ActorGroup");
        auto first = pipe.write_async(0, "first");
        first.get();
        auto second = pipe.write_async(0, "second");
        check(second.wait_for(0ms) == std::future_status::timeout,
              "pipeline write bypassed bounded backpressure");
        bool overloaded = false;
        try { pipe.write_async(0, "overflow").get(); }
        catch (const std::logic_error&) { overloaded = true; }
        check(overloaded, "pending writers bypassed bounded edge budget");
        check(pipe.read_async(0).get() == "first", "lost FIFO pipe message");
        second.get();
        check(pipe.read_async(0).get() == "second", "stalled writer did not resume");
        auto pending_read = pipe.read_async(1);
        check(pending_read.wait_for(0ms) == std::future_status::timeout,
              "empty edge must suspend receiver");
        bool read_overloaded = false;
        try { pipe.read_async(1).get(); }
        catch (const std::logic_error&) { read_overloaded = true; }
        check(read_overloaded, "pending readers bypassed bounded edge budget");
        pipe.write_async(1, "transformed").get();
        check(pending_read.get() == "transformed", "reader waiter missed delivery");
        auto buffered = pipe.write_async(0, "retained");
        buffered.get();
        auto blocked = pipe.write_async(0, "cancelled by shutdown");
        pipe.close();
        check(pipe.closed(), "native pipe did not close");
        bool failed = false;
        try { blocked.get(); } catch (const std::logic_error&) { failed = true; }
        check(failed, "pending writer stranded by native close");
        check(pipe.read_async(0).get() == "retained",
              "close discarded previously admitted payload");
        failed = false;
        try { pipe.read_async(0).get(); } catch (const std::logic_error&) { failed = true; }
        check(failed, "closed and drained edge permitted new read");
        check(registry.direct_group_ids(supervisor).empty(),
              "closed pipe retained supervisor's live group");
    }
    {
        NativeActorPipeTransport pipe(registry, supervisor, stages(), 8, 3);
        constexpr int count = 800;
        std::thread producer([&] {
            for (int i = 0; i < count; ++i)
                pipe.write_async(0, std::to_string(i)).get();
        });
        for (int i = 0; i < count; ++i)
            check(pipe.read_async(0).get() == std::to_string(i),
                  "concurrent native pipe lost FIFO ordering");
        producer.join();
        pipe.close();
    }
    // Close publishes its flag before acquiring every edge lock. A read
    // racing that walk may drain a buffered message, but MUST NOT turn a
    // previously queued write into an admitted message after publication.
    // Put the target edge last so the closure walk has intervening work.
    {
        std::vector<PipeStage> long_stages;
        long_stages.push_back({"Source", PipeRole::readable, "", "Message"});
        for (int i = 0; i < 256; ++i)
            long_stages.push_back({"Middle" + std::to_string(i),
                                   PipeRole::transform, "Message", "Message"});
        long_stages.push_back({"Sink", PipeRole::writable, "Message", ""});
        NativeActorPipeTransport pipe(registry, supervisor, long_stages, 1, 3);
        const auto last = pipe.edge_count() - 1;
        pipe.write_async(last, "admitted").get();
        auto pending = pipe.write_async(last, "must not be admitted");
        check(pending.wait_for(0ms) == std::future_status::timeout,
              "last-edge writer unexpectedly admitted before close");
        std::thread closer([&] { pipe.close(); });
        while (!pipe.closed()) std::this_thread::yield();
        std::string received;
        try {
            received = pipe.read_async(last).get();
        } catch (...) {
            closer.join();
            throw;
        }
        closer.join();
        check(received == "admitted",
              "shutdown failed to preserve previously admitted last-edge data");
        bool cancelled = false;
        try { pending.get(); } catch (const std::logic_error&) { cancelled = true; }
        check(cancelled, "shutdown promoted a queued last-edge writer");
    }
    bool rejected = false;
    NativeSupervisorRegistry foreign(10, 10, 10);
    try { NativeActorPipeTransport bad(registry, foreign.root(), stages(), 1, 3); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "foreign supervisor injected into native pipe");
    rejected = false;
    try { NativeActorPipeTransport bad(registry, supervisor, stages(), 0, 3); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "unbounded or zero capacity native pipe admitted");
}
