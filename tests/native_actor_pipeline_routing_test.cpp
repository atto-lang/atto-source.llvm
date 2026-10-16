#include "oreslang/runtime/NativeActorPipeTransport.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
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
        // Independent FNV-1a 64-bit known-slot fixtures also pinned by the
        // Java host routing smoke. Edges: [ConsumerA, ConsumerB].
        pipe.write_by_key_async("Producer", "partition-\\xf0\\x9f\\x8c\\x8d", "slot-zero").get();
        check(pipe.read_async(0).get() == "slot-zero",
              "UTF-8 partition fixture drifted from Java slot zero");
        pipe.write_by_key_async("Producer", "caf\\xc3\\xa9", "slot-one").get();
        check(pipe.read_async(1).get() == "slot-one",
              "accented UTF-8 partition fixture drifted from Java slot one");
        pipe.write_by_key_async("Producer", "device-42", "ascii-zero").get();
        check(pipe.read_async(0).get() == "ascii-zero",
              "ASCII partition fixture drifted from Java slot zero");
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


    // Empty data consumes queue/writer slots even though buffered bytes are 0.
    // Pending writes remain FIFO; excess admissions fail independently.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 2, 4);
        pipe.write_to_async("Producer", "ConsumerB", "").get();
        pipe.write_to_async("Producer", "ConsumerB", "").get();
        auto pending0 = pipe.write_to_async("Producer", "ConsumerB", "");
        auto pending1 = pipe.write_to_async("Producer", "ConsumerB", "");
        check(pending0.wait_for(0ms) == std::future_status::timeout &&
              pending1.wait_for(0ms) == std::future_status::timeout,
              "zero-byte writer unexpectedly bypassed queue slots");
        bool denied_empty = false;
        try { pipe.write_to_async("Producer", "ConsumerB", "").get(); }
        catch (const std::logic_error&) { denied_empty = true; }
        check(denied_empty, "zero-byte payload bypassed bounded pending writers");
        check(pipe.read_async(1).get().empty() && pipe.read_async(1).get().empty(),
              "admitted empty messages missing");
        pending0.get(); pending1.get();
        check(pipe.read_async(1).get().empty() && pipe.read_async(1).get().empty(),
              "pending empty-message queue did not drain");
        pipe.close();
    }

    // 16-MiB payload credits alone cannot limit empty-message metadata.
    // Reject malformed or excessive queue config before allocating edges.
    {
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, graph_stages(), graph_edges(), 0, 4);
        }, "zero edge capacity accepted");
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, graph_stages(), graph_edges(), 4097, 4);
        }, "unbounded empty-message capacity accepted");
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, graph_stages(), graph_edges(),
                std::numeric_limits<std::size_t>::max(), 4);
        }, "size_t overflow capacity was not rejected");
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, graph_stages(), graph_edges(), 4, 0);
        }, "zero worker capacity accepted");
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, graph_stages(), graph_edges(), 4, 4097);
        }, "unbounded worker capacity accepted");

        std::vector<PipeStage> many = {
            {"Producer", PipeRole::readable, "", "Event"}
        };
        std::vector<PipeConnection> links;
        for (int i = 0; i < 17; ++i) {
            std::string sink = "sink-" + std::to_string(i);
            many.push_back({"Worker", PipeRole::writable, "Event", "", sink});
            links.push_back({"Producer", sink, "Event"});
        }
        rejected([&] {
            NativeActorPipeTransport pipe(
                registry, supervisor, many, links, 4096, 4);
        }, "aggregate empty-message slot amplification accepted");
        // Boundary: exactly 16 * 4,096 = 65,536 graph slots is valid.
        many.pop_back();
        links.pop_back();
        {
            NativeActorPipeTransport pipe(
                registry, supervisor, many, links, 4096, 4);
            check(pipe.edge_count() == 16,
                  "legal aggregate slot boundary rejected");
            pipe.close();
        }
    }

    // Graph-wide resident-payload budget is shared across otherwise healthy
    // edges. Per-edge limits alone would permit 4096 * 16 MiB = 64 GiB.
    {
        std::vector<PipeStage> stages = {
            {"Producer", PipeRole::readable, "", "Event"}
        };
        std::vector<PipeConnection> links;
        for (int i = 0; i < 5; ++i) {
            const std::string name = "consumer-" + std::to_string(i);
            stages.push_back({"Worker", PipeRole::writable, "Event", "", name});
            links.push_back({"Producer", name, "Event"});
        }
        NativeActorPipeTransport pipe(registry, supervisor, stages, links, 32, 4);
        const std::string megabyte(1024 * 1024, 'G');
        for (int e = 0; e < 4; ++e) {
            for (int i = 0; i < 16; ++i)
                pipe.write_to_async("Producer",
                    "consumer-" + std::to_string(e), megabyte).get();
        }
        check(pipe.buffered_payload_bytes() == 64ULL * 1024 * 1024,
              "graph payload telemetry did not reflect full budget");
        // Every individual edge still has credits, but the graph has none.
        auto receipts = pipe.write_all_report_async("Producer", megabyte);
        check(receipts.size() == 5, "graph-credit fanout dropped a receipt");
        for (auto& entry : receipts) {
            bool exhausted = false;
            try { entry.admission.get(); }
            catch (const std::logic_error&) { exhausted = true; }
            check(exhausted, "global credit was bypassed by fanout");
        }
        bool graph_full = false;
        try { pipe.write_to_async("Producer", "consumer-4", megabyte).get(); }
        catch (const std::logic_error&) { graph_full = true; }
        check(graph_full, "healthy edge bypassed global byte budget");
        // Direct handoff retains no bytes: a ready consumer can still
        // receive even when every buffer credit is exhausted. Fanout must
        // report the one success and four failures independently.
        auto waiting = pipe.read_async(4);
        auto partial = pipe.write_all_report_async("Producer", megabyte);
        check(partial.size() == 5,
              "graph-exhausted direct-handoff fanout lost receipt labels");
        for (std::size_t i = 0; i < partial.size(); ++i) {
            bool admitted = true;
            try { partial[i].admission.get(); }
            catch (const std::logic_error&) { admitted = false; }
            check(admitted == (i == 4),
                  "graph-wide pressure misreported independently admitted fanout");
        }
        check(waiting.get() == megabyte &&
              pipe.buffered_payload_bytes() == 64ULL * 1024 * 1024,
              "zero-buffer direct delivery consumed graph payload credits");
        // Consuming one queued message returns its graph-wide credit.
        check(pipe.read_async(0).get() == megabyte, "graph credit drain lost data");
        check(pipe.buffered_payload_bytes() == 63ULL * 1024 * 1024,
              "cross-edge read did not return graph credit");
        pipe.write_to_async("Producer", "consumer-4", megabyte).get();
        check(pipe.read_async(4).get() == megabyte,
              "global byte credit was not returned to another edge");
        check(pipe.buffered_payload_bytes() == 63ULL * 1024 * 1024,
              "global credit was double-charged across edge transfer");
        // After STOP, admitted messages remain readable but new writes fail.
        pipe.close();
        bool stopped = false;
        try { pipe.write_to_async("Producer", "consumer-4", "late").get(); }
        catch (const std::logic_error&) { stopped = true; }
        check(stopped, "STOP reopened global byte credit for new writes");
    }

    // Stress concurrent graph-global credit reservations. Even if all
    // producers race to different edges, at most 64 * 1 MiB may be retained.
    {
        std::vector<PipeStage> stages = {
            {"Producer", PipeRole::readable, "", "Event"}
        };
        std::vector<PipeConnection> links;
        for (int i = 0; i < 5; ++i) {
            const std::string target = "race-" + std::to_string(i);
            stages.push_back({"Worker", PipeRole::writable, "Event", "", target});
            links.push_back({"Producer", target, "Event"});
        }
        NativeActorPipeTransport pipe(registry, supervisor, stages, links, 32, 4);
        const std::string megabyte(1024 * 1024, 'R');
        std::atomic<int> success{0};
        std::atomic<int> failures{0};
        std::atomic<int> edge_success[5] = {};
        std::vector<std::thread> senders;
        for (int worker = 0; worker < 8; ++worker) {
            senders.emplace_back([&, worker] {
                for (int i = 0; i < 25; ++i) {
                    const int target = (worker * 25 + i) % 5;
                    try {
                        pipe.write_to_async("Producer",
                            "race-" + std::to_string(target), megabyte).get();
                        ++success;
                        ++edge_success[target];
                    } catch (const std::logic_error&) {
                        ++failures;
                    }
                }
            });
        }
        for (auto& sender : senders) sender.join();
        check(success.load() == 64 && failures.load() == 136,
              "racing writers bypassed or prematurely exhausted graph credits");
        check(pipe.buffered_payload_bytes() == 64ULL * 1024 * 1024,
              "racing admissions did not reserve exactly one global byte credit");
        for (int edge = 0; edge < 5; ++edge) {
            for (int i = 0; i < edge_success[edge].load(); ++i)
                check(pipe.read_async(static_cast<std::size_t>(edge)).get() == megabyte,
                      "concurrent global-credit delivery dropped payload");
        }
        check(pipe.buffered_payload_bytes() == 0,
              "concurrent graph-credit read path leaked reservations");
        // Refill to prove every drained reservation becomes available again.
        for (int i = 0; i < 16; ++i)
            pipe.write_to_async("Producer", "race-0", megabyte).get();
        for (int i = 0; i < 16; ++i)
            check(pipe.read_async(0).get() == megabyte,
                  "global-credit race left leaked reservations after draining");
        check(pipe.buffered_payload_bytes() == 0,
              "refilled queue did not return all graph reservations");
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


#if defined(ORESLANG_ACTOR_PIPE_FAULT_TESTS)
    // Deterministic bad_alloc at the deque-promotion boundary. A reader has
    // already consumed an admitted payload; the failed pending writer must
    // receive a terminal exception and release both its byte reservations.
    {
        NativeActorPipeTransport pipe(
            registry, supervisor, graph_stages(), graph_edges(), 1, 4);
        pipe.write_to_async("Producer", "ConsumerB", "ready").get();
        auto pending = pipe.write_to_async("Producer", "ConsumerB", "queued");
        check(pending.wait_for(0ms) == std::future_status::timeout,
              "promotion fault fixture did not create a pending writer");
        check(pipe.buffered_payload_bytes() == 11,
              "promotion fault fixture charged wrong initial bytes");
        pipe.fail_next_writer_promotion_for_test();
        check(pipe.read_async(1).get() == "ready",
              "promotion allocation failure lost an admitted reader message");
        bool promotion_failed = false;
        try { pending.get(); }
        catch (const std::bad_alloc&) { promotion_failed = true; }
        check(promotion_failed,
              "pending writer did not receive its promotion allocation failure");
        check(pipe.buffered_payload_bytes() == 0,
              "failed promotion leaked graph buffered-byte reservation");
        pipe.write_to_async("Producer", "ConsumerB", "reuse").get();
        check(pipe.read_async(1).get() == "reuse",
              "failed promotion poisoned subsequent edge admissions");
        check(pipe.buffered_payload_bytes() == 0,
              "reused mailbox did not release its charged payload");
        pipe.close();
    }
#endif

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
        check(pipe.buffered_payload_bytes() == 16ULL * 1024 * 1024,
              "pending writers were not charged against graph payload credit");
        bool denied_writer = false;
        try { pipe.write_to_async("Producer", "ConsumerB", megabyte).get(); }
        catch (const std::logic_error&) { denied_writer = true; }
        check(denied_writer, "pending writers bypassed resident byte cap");
        check(pipe.read_async(1).get() == megabyte,
              "byte-credit drain missed queued payload");
        check(pipe.buffered_payload_bytes() == 15ULL * 1024 * 1024,
              "promoting a pending writer duplicated or leaked byte credit");
        pending_writers[0].get();
        auto recovered = pipe.write_to_async("Producer", "ConsumerB", megabyte);
        check(recovered.wait_for(0ms) == std::future_status::timeout,
              "byte-credit recovery should still respect mailbox backpressure");
        check(pipe.buffered_payload_bytes() == 16ULL * 1024 * 1024,
              "recovered pending writer was not charged exactly once");
        pipe.close();
        check(pipe.buffered_payload_bytes() == 10ULL * 1024 * 1024,
              "STOP did not release credits for cancelled pending writers");
        bool cancelled = false;
        try { recovered.get(); }
        catch (const std::logic_error&) { cancelled = true; }
        check(cancelled, "STOP failed to cancel byte-accounted pending writer");
        for (int i = 0; i < 10; ++i)
            check(pipe.read_async(1).get() == megabyte,
                  "post-STOP drain lost an already-admitted message");
        check(pipe.buffered_payload_bytes() == 0,
              "post-STOP drain leaked graph payload credit");
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
