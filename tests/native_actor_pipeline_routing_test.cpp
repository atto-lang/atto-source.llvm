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

static void check(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
template <class F>
static void rejected(F action, const char* message) {
    bool denied = false;
    try { action(); } catch (const std::invalid_argument&) { denied = true; }
    check(denied, message);
}
static std::vector<PipeStage> graph_stages() {
    return {{"Producer", PipeRole::readable, "", "Event"},
            {"ConsumerA", PipeRole::writable, "Event", ""},
            {"ConsumerB", PipeRole::writable, "Event", ""}};
}
static std::vector<PipeConnection> graph_edges() {
    return {{"Producer", "ConsumerA", "Event"},
            {"Producer", "ConsumerB", "Event"}};
}
static std::size_t key_slot(const std::string& key) {
    std::uint64_t digest = UINT64_C(14695981039346656037);
    for (unsigned char c : key) {
        digest ^= c;
        digest *= UINT64_C(1099511628211);
    }
    return static_cast<std::size_t>(digest % 2);
}

int main() {
    NativeSupervisorRegistry registry(25, 25, 25);
    auto supervisor = registry.create_child(registry.root());

    // A real checked DAG, not a fictitious list of linear pipe edges.
    {
        auto links = check_actor_pipeline_graph(graph_stages(), graph_edges());
        check(links.size() == 2, "lost fan-out graph edges");
        rejected([&] {
            auto edges = graph_edges();
            edges[1].payload = "Wrong";
            (void)check_actor_pipeline_graph(graph_stages(), edges);
        }, "mismatched payload permitted");
        rejected([&] {
            auto edges = graph_edges();
            edges.push_back(edges[0]);
            (void)check_actor_pipeline_graph(graph_stages(), edges);
        }, "duplicate edge permitted");
        rejected([&] {
            auto edges = graph_edges();
            edges[1].target = "NotConnected";
            (void)check_actor_pipeline_graph(graph_stages(), edges);
        }, "unknown destination permitted");
        rejected([&] {
            auto stages = graph_stages();
            stages[2].actor_type = "ConsumerA";
            (void)check_actor_pipeline_graph(stages, graph_edges());
        }, "duplicate actor type permitted");
        rejected([&] {
            auto stages = graph_stages();
            stages[1] = {"ConsumerA", PipeRole::transform, "Event", "Event"};
            stages[2] = {"ConsumerB", PipeRole::duplex, "Event", "Event"};
            auto links = graph_edges();
            links.push_back({"ConsumerA", "ConsumerB", "Event"});
            links.push_back({"ConsumerB", "ConsumerA", "Event"});
            (void)check_actor_pipeline_graph(stages, links);
        }, "cycle permitted");
        rejected([&] {
            auto stages = graph_stages();
            stages[1].role = PipeRole::supervisor_actor;
            (void)check_actor_pipeline_graph(stages, graph_edges());
        }, "supervisor actor in data pipeline permitted");
    }

    // Distinct instances of the SAME worker class are separate connected
    // consumers. Both multicast and deterministic load spread must work.
    {
        std::vector<PipeStage> stages = {
            {"SourceType", PipeRole::readable, "", "Event", "source-1"},
            {"Worker", PipeRole::writable, "Event", "", "worker-a"},
            {"Worker", PipeRole::writable, "Event", "", "worker-b"}
        };
        std::vector<PipeConnection> edges = {
            {"source-1", "worker-a", "Event"},
            {"source-1", "worker-b", "Event"}
        };
        NativeActorPipeTransport pipe(registry, supervisor, stages, edges, 2, 4);
        auto batch = pipe.write_all_report_async("source-1", "fanout");
        check(batch.size() == 2 && batch[0].target == "worker-a" &&
              batch[1].target == "worker-b",
              "same-type worker identities were lost");
        for (auto& item : batch) item.admission.get();
        check(pipe.read_async(0).get() == "fanout", "worker-a missed fanout");
        check(pipe.read_async(1).get() == "fanout", "worker-b missed fanout");
        pipe.write_round_robin_async("source-1", "first").get();
        pipe.write_round_robin_async("source-1", "second").get();
        check(pipe.read_async(0).get() == "first", "same-type RR first wrong");
        check(pipe.read_async(1).get() == "second", "same-type RR second wrong");
        pipe.write_to_async("source-1", "worker-b", "direct").get();
        check(pipe.read_async(1).get() == "direct", "instance target misrouted");
        rejected([&] { pipe.write_to_async("source-1", "Worker", "bad"); },
                 "nominal class name bypassed instance connection authority");
        pipe.close();
    }

    // Legacy one-consumer source forward is explicit, not policy-dependent.
    {
        std::vector<PipeStage> linear = {
            {"Producer", PipeRole::readable, "", "Event"},
            {"ConsumerA", PipeRole::writable, "Event", ""}
        };
        NativeActorPipeTransport pipe(registry, supervisor, linear, 2, 4);
        pipe.write_one_async("Producer", "single").get();
        check(pipe.read_async(0).get() == "single",
              "single-edge default forwarding failed");
        rejected([&] { (void)pipe.write_one_async("ConsumerA", "bad"); },
                 "unconnected actor was allowed to forward");
        pipe.close();
    }

    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 2, 4);
        check(pipe.edge_count() == 2, "graph did not create two bounded edges");
        rejected([&] { (void)pipe.write_one_async("Producer", "ambiguous"); },
                 "ambiguous forward chose an arbitrary destination");
        auto tagged = pipe.write_all_report_async("Producer", "tagged");
        check(tagged.size() == 2 && tagged[0].edge_index == 0 &&
              tagged[0].target == "ConsumerA" && tagged[1].edge_index == 1 &&
              tagged[1].target == "ConsumerB",
              "fanout reports lost stable edge identity");
        for (auto& entry : tagged) entry.admission.get();
        check(pipe.read_async(0).get() == "tagged", "tagged fanout first missing");
        check(pipe.read_async(1).get() == "tagged", "tagged fanout second missing");
        auto ack = pipe.write_all_async("Producer", "broadcast");
        check(ack.size() == 2, "fan-out skipped a recipient");
        for (auto& result : ack) result.get();
        check(pipe.read_async(0).get() == "broadcast", "first consumer missed fan-out");
        check(pipe.read_async(1).get() == "broadcast", "second consumer missed fan-out");

        // Selection order is per producer; even if an edge blocks later, its
        // turn does not silently reroute to another actor.
        pipe.write_round_robin_async("Producer", "round-0").get();
        pipe.write_round_robin_async("Producer", "round-1").get();
        pipe.write_round_robin_async("Producer", "round-2").get();
        check(pipe.read_async(0).get() == "round-0", "round-robin first wrong");
        check(pipe.read_async(1).get() == "round-1", "round-robin second wrong");
        check(pipe.read_async(0).get() == "round-2", "round-robin wrap wrong");

        pipe.write_to_async("Producer", "ConsumerB", "direct").get();
        check(pipe.read_async(1).get() == "direct", "named delivery wrong");
        rejected([&] {
            pipe.write_to_async("Producer", "ConsumerMissing", "invalid").get();
        }, "unknown destination should not be routable");
        rejected([&] {
            pipe.write_to_async("ConsumerA", "ConsumerB", "invalid").get();
        }, "nonproducer must not route using another actor's connection");

        for (int i = 0; i < 4; ++i) {
            const auto selected = key_slot("stable-partition");
            pipe.write_by_key_async("Producer", "stable-partition", "hash").get();
            check(pipe.read_async(selected).get() == "hash",
                  "key affinity changed between sends");
        }
        rejected([&] {
            pipe.write_by_key_async("Producer", std::string(4097, 'x'), "bad").get();
        }, "unbounded route key permitted");
        rejected([&] {
            pipe.write_by_key_async("Producer", std::string("\xc0\xaf", 2), "bad").get();
        }, "overlong UTF-8 routing key permitted");
        rejected([&] {
            pipe.write_by_key_async("Producer", std::string("\xed\xa0\x80", 3), "bad").get();
        }, "UTF-8 surrogate routing key permitted");
        rejected([&] {
            pipe.write_by_key_async("Producer", std::string("\xf4\x90\x80\x80", 4), "bad").get();
        }, "UTF-8 out-of-range routing key permitted");
        rejected([&] {
            pipe.write_by_key_async("Producer", std::string("\xe2\x82", 2), "bad").get();
        }, "truncated UTF-8 routing key permitted");
        const std::string unicode_key("\xf0\x9f\x8c\x8d", 4);
        pipe.write_by_key_async("Producer", unicode_key, "valid-unicode").get();
        check(pipe.read_async(key_slot(unicode_key)).get() == "valid-unicode",
              "valid non-ASCII UTF-8 key was misrouted");
        pipe.close();
        bool denied = false;
        try { pipe.write_round_robin_async("Producer", "after-close").get(); }
        catch (const std::logic_error&) { denied = true; }
        check(denied, "STOP did not fence further routed writes");
    }

    // Explicit non-transactional fanout: caller gets a Future per recipient,
    // so partial delivery cannot silently masquerade as a successful batch.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 1, 4);
        pipe.write_to_async("Producer", "ConsumerB", "full").get();
        auto pending = pipe.write_to_async("Producer", "ConsumerB", "queued");
        check(pending.wait_for(0ms) == std::future_status::timeout,
              "expected destination pressure");
        const auto receipts = pipe.write_all_report_async("Producer", "mixed");
        check(receipts.size() == 2 && receipts[0].target == "ConsumerA" &&
              receipts[1].target == "ConsumerB",
              "partial fanout lost target-labelled receipts");
        receipts[0].admission.get();
        bool failed_second = false;
        try { receipts[1].admission.get(); }
        catch (const std::logic_error&) { failed_second = true; }
        check(failed_second, "fanout hid failed overloaded branch");
        check(pipe.read_async(0).get() == "mixed", "healthy branch not delivered");
        pipe.close();
        bool pending_cancelled = false;
        try { pending.get(); } catch (const std::logic_error&) { pending_cancelled = true; }
        check(pending_cancelled, "pending branch was not cancelled on STOP");
    }

    // Every RR attempt consumes its turn, including an overloaded edge.
    // Pending admission must not silently skip to a healthier consumer.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 1, 4);
        pipe.write_round_robin_async("Producer", "a0").get();
        pipe.write_round_robin_async("Producer", "b0").get();
        auto queued_a = pipe.write_round_robin_async("Producer", "a1");
        auto queued_b = pipe.write_round_robin_async("Producer", "b1");
        check(queued_a.wait_for(0ms) == std::future_status::timeout &&
              queued_b.wait_for(0ms) == std::future_status::timeout,
              "RR must preserve per-edge backpressure");
        bool rejected_a = false, rejected_b = false;
        try { pipe.write_round_robin_async("Producer", "a2").get(); }
        catch (const std::logic_error&) { rejected_a = true; }
        try { pipe.write_round_robin_async("Producer", "b2").get(); }
        catch (const std::logic_error&) { rejected_b = true; }
        check(rejected_a && rejected_b,
              "RR must report saturated admissions independently");
        check(pipe.read_async(0).get() == "a0" &&
              pipe.read_async(1).get() == "b0",
              "RR saturation corrupted admitted messages");
        queued_a.get();
        queued_b.get();
        check(pipe.read_async(0).get() == "a1" &&
              pipe.read_async(1).get() == "b1",
              "RR pending admissions did not drain in order");
        pipe.write_round_robin_async("Producer", "next-a").get();
        check(pipe.read_async(0).get() == "next-a",
              "RR cursor did not consume rejected turns");
        pipe.close();
    }

    // A failed fanout returns a target-labelled failure for every branch,
    // not an empty result or one ambiguous aggregate exception.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 1, 4);
        auto report = pipe.write_all_report_async(
            "Producer", std::string(1024 * 1024 + 1, 'x'));
        check(report.size() == 2 && report[0].target == "ConsumerA" &&
              report[1].target == "ConsumerB",
              "oversized fanout lost per-target receipts");
        for (auto& receipt : report) {
            bool denied = false;
            try { receipt.admission.get(); }
            catch (const std::logic_error&) { denied = true; }
            check(denied, "oversized fanout edge did not reject admission");
        }
        pipe.close();
    }


    // Per-edge byte credit applies to accepted messages and to pending
    // writes, not just to the maximum size of one individual payload.
    // Exhausting one branch must not consume another branch's byte credit.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 32, 4);
        const std::string megabyte(1024 * 1024, 'B');
        for (int i = 0; i < 16; ++i)
            pipe.write_to_async("Producer", "ConsumerB", megabyte).get();
        bool denied_bytes = false;
        try {
            pipe.write_to_async("Producer", "ConsumerB", megabyte).get();
        } catch (const std::logic_error&) { denied_bytes = true; }
        check(denied_bytes, "edge accepted more than 16 MiB resident bytes");

        auto batch = pipe.write_all_report_async("Producer", megabyte);
        check(batch.size() == 2 && batch[0].target == "ConsumerA" &&
              batch[1].target == "ConsumerB",
              "byte-exhausted branch lost stable fanout receipts");
        batch[0].admission.get();
        bool rejected_branch = false;
        try { batch[1].admission.get(); }
        catch (const std::logic_error&) { rejected_branch = true; }
        check(rejected_branch, "fanout concealed the exhausted byte-credit branch");
        check(pipe.read_async(0).get() == megabyte,
              "healthy branch failed when peer byte credit was exhausted");

        check(pipe.read_async(1).get() == megabyte,
              "byte credit could not be released by consuming a message");
        pipe.write_to_async("Producer", "ConsumerB", megabyte).get();
        for (int i = 0; i < 16; ++i)
            check(pipe.read_async(1).get() == megabyte,
                  "byte-credit recovery corrupted per-edge FIFO");
        pipe.close();
    }


    // Pending writers consume the SAME byte credit as accepted messages.
    // Promotion from a pending writer to the mailbox transfers (rather
    // than duplicates) that credit, and the read frees one message's bytes.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 10, 4);
        const std::string megabyte(1024 * 1024, 'Q');
        for (int i = 0; i < 10; ++i)
            pipe.write_to_async("Producer", "ConsumerB", megabyte).get();
        std::vector<std::shared_future<void>> pending_writers;
        for (int i = 0; i < 6; ++i) {
            auto future = pipe.write_to_async("Producer", "ConsumerB", megabyte);
            check(future.wait_for(0ms) == std::future_status::timeout,
                  "saturated mailbox unexpectedly admitted pending writer");
            pending_writers.push_back(std::move(future));
        }
        bool denied_writer = false;
        try { pipe.write_to_async("Producer", "ConsumerB", megabyte).get(); }
        catch (const std::logic_error&) { denied_writer = true; }
        check(denied_writer, "pending writers bypassed resident byte cap");
        check(pipe.read_async(1).get() == megabyte,
              "byte-credit drain missed queued payload");
        pending_writers[0].get();
        auto recovered = pipe.write_to_async("Producer", "ConsumerB", megabyte);
        check(recovered.wait_for(0ms) == std::future_status::timeout,
              "byte-credit recovery should still respect mailbox backpressure");
        pipe.close();
        bool cancelled = false;
        try { recovered.get(); }
        catch (const std::logic_error&) { cancelled = true; }
        check(cancelled, "STOP failed to cancel byte-accounted pending writer");
    }

    // Host-only concurrency: per-source selector mutex serializes selection,
    // while actual actor guest execution is outside this transport.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 1024, 4);
        constexpr int per_producer = 400;
        std::atomic<int> failed{0};
        std::vector<std::thread> senders;
        for (int p = 0; p < 4; ++p) {
            senders.emplace_back([&, p] {
                for (int n = 0; n < per_producer; ++n) {
                    try {
                        pipe.write_round_robin_async("Producer",
                            std::to_string(p) + ":" + std::to_string(n)).get();
                    } catch (...) { ++failed; }
                }
            });
        }
        for (auto& sender : senders) sender.join();
        check(failed.load() == 0, "unexpected pressure under bounded stress");
        for (int i = 0; i < 800; ++i) {
            check(!pipe.read_async(0).get().empty(), "missing even slot");
            check(!pipe.read_async(1).get().empty(), "missing odd slot");
        }
        pipe.close();
    }
}
