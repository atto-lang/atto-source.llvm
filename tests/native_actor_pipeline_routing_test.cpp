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

    // Two downstream instances of ONE actor class must have independent
    // destinations and mailboxes. Graph edges identify node instances, never
    // a class name shared by several workers.
    {
        const std::vector<PipeStage> instances = {
            {"Ingress", PipeRole::readable, "", "Event", "front"},
            {"Worker", PipeRole::writable, "Event", "", "worker-0"},
            {"Worker", PipeRole::writable, "Event", "", "worker-1"}
        };
        const std::vector<PipeConnection> edges = {
            {"front", "worker-0", "Event"},
            {"front", "worker-1", "Event"}
        };
        check(check_actor_pipeline_graph(instances, edges).size() == 2,
              "same-class consumer instances rejected");
        NativeActorPipeTransport pipe(
            registry, supervisor, instances, edges, 2, 4);
        pipe.write_round_robin_async("front", "job-0").get();
        pipe.write_round_robin_async("front", "job-1").get();
        check(pipe.read_async(0).get() == "job-0",
              "first worker instance received wrong job");
        check(pipe.read_async(1).get() == "job-1",
              "second worker instance received wrong job");
        pipe.write_to_async("front", "worker-1", "named").get();
        check(pipe.read_async(1).get() == "named",
              "named route confused worker class with node instance");
        rejected([&] {
            pipe.write_to_async("front", "Worker", "ambiguous");
        }, "actor class name leaked as implicit route authority");
        rejected([&] {
            auto bad = instances;
            bad[2].node_id = "worker-0";
            (void)check_actor_pipeline_graph(bad, edges);
        }, "duplicate instance ID permitted");
        rejected([&] {
            auto bad = instances;
            bad[2].node_id = "front";
            (void)check_actor_pipeline_graph(bad, edges);
        }, "node ID alias to source permitted");
        pipe.close();
    }

    // Linear pipes also require unique node identities when actor types
    // repeat; otherwise subsequent node-oriented routing would alias edges.
    {
        const std::vector<PipeStage> repeated = {
            {"Worker", PipeRole::readable, "", "Event", "one"},
            {"Worker", PipeRole::writable, "Event", "", "two"}
        };
        const auto links = check_actor_pipeline(repeated);
        check(links.size() == 1 && links[0].source == "one" &&
              links[0].target == "two",
              "linear same-class actors lost explicit instance IDs");
        rejected([&] {
            auto duplicate = repeated;
            duplicate[0].node_id.clear();
            duplicate[1].node_id.clear();
            (void)check_actor_pipeline(duplicate);
        }, "linear duplicate actor identities were conflated");
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
