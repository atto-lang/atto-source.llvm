#include "oreslang/runtime/NativeActorPipeTransport.hpp"

#include <atomic>
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
    // A second close caller must not return with unsettled pending Futures,
    // even when closure traverses a large number of independently locked
    // mailbox edges. Concurrent write admission must be fenced afterward.
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::vector<PipeStage> many;
        many.push_back({"Start", PipeRole::readable, "", "Message"});
        for (int i = 0; i < 32; ++i)
            many.push_back({"Relay" + std::to_string(i),
                            PipeRole::transform, "Message", "Message"});
        many.push_back({"End", PipeRole::writable, "Message", ""});
        NativeActorPipeTransport pipe(registry, supervisor, many, 1, 3);
        std::vector<std::shared_future<void>> pending;
        for (std::size_t i = 0; i < pipe.edge_count(); ++i) {
            pipe.write_async(i, "existing").get();
            pending.push_back(pipe.write_async(i, "blocked"));
        }
        std::atomic<bool> start{false};
        std::thread first([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            pipe.close();
        });
        std::thread second([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            pipe.close();
        });
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        check(pipe.closed(), "concurrent pipe close failed to publish terminal");
        for (std::size_t i = 0; i < pipe.edge_count(); ++i) {
            check(pending[i].wait_for(0ms) == std::future_status::ready,
                  "concurrent close returned with unsettled pending write");
            bool denied = false;
            try { pending[i].get(); }
            catch (const std::logic_error&) { denied = true; }
            check(denied, "closed pipe admitted a previously blocked writer");
            check(pipe.read_async(i).get() == "existing",
                  "concurrent close erased previously acknowledged data");
            denied = false;
            try { pipe.write_async(i, "after-close").get(); }
            catch (const std::logic_error&) { denied = true; }
            check(denied, "pipe admitted new writes after terminal close");
        }
    }

    // Atomic receive-turn batch parity: all messages to ONE edge are
    // admitted together, or no prefix/credit is published. Do not confuse
    // this with non-transactional multi-edge forward_all.
    {
        NativeActorPipeTransport pipe(registry, supervisor, stages(), 3, 3,
                                      8, 3);
        check(pipe.try_write_turn_atomic(0, {"a", "bb"}),
              "valid two-message turn rejected");
        check(pipe.buffered_message_count() == 2
                  && pipe.buffered_payload_bytes() == 3,
              "atomic batch counters not committed");
        check(!pipe.try_write_turn_atomic(0, {"ccc", "dddd"}),
              "oversized batch admitted a prefix");
        check(pipe.buffered_message_count() == 2
                  && pipe.buffered_payload_bytes() == 3,
              "rejected batch consumed graph credits");
        check(pipe.read_async(0).get() == "a", "batch FIFO first missing");
        check(pipe.read_async(0).get() == "bb", "batch FIFO second missing");
        check(pipe.buffered_message_count() == 0
                  && pipe.buffered_payload_bytes() == 0,
              "batch receiver did not refund graph credits");
        check(pipe.try_write_turn_atomic(0, {"one", "two", "x"}),
              "released credits were not reusable for a new actor turn");
        check(!pipe.try_write_turn_atomic(0, {"z"}),
              "full batch mailbox accepted a fourth message");
        pipe.close();
        check(pipe.read_async(0).get() == "one", "close erased first admitted DATA");
        check(pipe.read_async(0).get() == "two", "close erased second admitted DATA");
        check(pipe.read_async(0).get() == "x", "close erased third admitted DATA");
        bool denied = false;
        try { (void)pipe.try_write_turn_atomic(0, {"later"}); }
        catch (const std::logic_error&) { denied = true; }
        check(denied, "STOP admitted a new actor turn");
    }
    {
        // Existing reader waiter must not be skipped by an atomic batch.
        NativeActorPipeTransport pipe(registry, supervisor, stages(), 3, 3);
        auto pending_read = pipe.read_async(0);
        check(!pipe.try_write_turn_atomic(0, {"bypassed"}),
              "atomic batch bypassed an earlier pending reader");
        pipe.write_async(0, "first").get();
        check(pending_read.get() == "first", "waiting reader missed first DATA");
        pipe.close();
    }
    for (int attempt = 0; attempt < 128; ++attempt) {
        NativeActorPipeTransport pipe(registry, supervisor, stages(), 3, 3);
        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            try { (void)pipe.try_write_turn_atomic(0, {"a", "b"}); }
            catch (const std::logic_error&) { }
        });
        std::thread stopper([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            pipe.close();
        });
        go.store(true, std::memory_order_release);
        producer.join();
        stopper.join();
        check(pipe.closed(), "concurrent batch/STOP missed terminal seal");
        check(pipe.buffered_message_count() == 0
                  || pipe.buffered_message_count() == 2,
              "concurrent STOP admitted a partial message batch");
        bool denied = false;
        try { (void)pipe.try_write_turn_atomic(0, {"after-stop"}); }
        catch (const std::logic_error&) { denied = true; }
        check(denied, "STOP admitted new batch");
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
