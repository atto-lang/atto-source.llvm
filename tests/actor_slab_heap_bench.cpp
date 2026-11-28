// Memory comparison: the same actor object graph stored four ways.
//
//   malloc64 : one `new` per node, native-width pointers (common native baseline)
//   arena64  : bump arena per actor, native-width pointers (pointer width only)
//   offset32 : ActorSlabHeap, Offset32 edges (compiler-proven lifetimes)
//   handle32 : ActorSlabHeap, Handle32 edges (stale-reference protection)
//
// Node = { int64 payload; ref left; ref right; }. Each run builds `actors`
// actor heaps holding `nodes` nodes each, links every node to two earlier
// nodes, touches everything, then STOPs every actor. One variant per process,
// so RSS high-water marks do not leak between variants.
//
// usage: actor_slab_heap_bench <variant> <actors> <nodes_per_actor>
// prints: variant actors nodes ptr_bits rss_built_kib rss_after_stop_kib
//         bytes_per_node alloc_ns_avg alloc_ns_max metadata_kib
#include "oreslang/runtime/ActorSlabHeap.hpp"
// -DORES_BENCH_SMALL_PAGES measures the 1 KiB small-actor page profile.
#if defined(ORES_BENCH_SMALL_PAGES)
using BenchHeap = oreslang::runtime::ActorSlabHeapSmall;
using BenchPool = oreslang::runtime::ActorHeapRegionPoolSmall;
#else
using BenchHeap = oreslang::runtime::ActorSlabHeap;
using BenchPool = oreslang::runtime::ActorHeapRegionPool;
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace oreslang::runtime;

static std::size_t rss_kib() {
#if defined(__linux__)
    FILE* f = std::fopen("/proc/self/statm", "r");
    unsigned long size = 0, resident = 0;
    if (f) { if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0; std::fclose(f); }
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE)) / 1024;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return static_cast<std::size_t>(info.resident_size / 1024);
#else
    return 0;
#endif
}

struct NodeNative { std::int64_t payload; NodeNative* left; NodeNative* right; };
struct NodeCompact { std::int64_t payload; std::uint32_t left; std::uint32_t right; };
static_assert(sizeof(NodeCompact) == 16, "compact node is 16 bytes on every host");

struct Timing {
    double total_ns = 0; double max_ns = 0; std::size_t n = 0;
    template<class F> auto time(F&& f) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = f();
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        total_ns += ns; max_ns = std::max(max_ns, ns); ++n;
        return r;
    }
};

static std::uint32_t pick(std::uint32_t i, std::uint32_t salt) {
    return i == 0 ? 0 : (i * 2654435761u + salt) % i; // an earlier node
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s variant actors nodes\n", argv[0]); return 2; }
    const std::string variant = argv[1];
    const auto actors = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const auto nodes = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const std::size_t base_rss = rss_kib();
    Timing timing;
    std::size_t metadata = 0;
    std::int64_t checksum = 0;
    std::size_t rss_built = 0;

    if (variant == "malloc64") {
        std::vector<std::vector<NodeNative*>> all(actors);
        for (auto& actor : all) {
            actor.resize(nodes);
            for (std::uint32_t i = 0; i < nodes; ++i) {
                auto* n = timing.time([] { return new NodeNative{}; });
                n->payload = i;
                n->left = actor[pick(i, 1)];
                n->right = actor[pick(i, 7)];
                actor[i] = n;
            }
            metadata += actor.capacity() * sizeof(void*); // the root table, not per-node
        }
        for (auto& actor : all) for (auto* n : actor) checksum += n->payload + (n->left ? n->left->payload : 0);
        rss_built = rss_kib();
        for (auto& actor : all) { for (auto* n : actor) delete n; std::vector<NodeNative*>().swap(actor); }
    } else if (variant == "arena64") {
        std::vector<std::unique_ptr<NodeNative[]>> all(actors);
        for (auto& actor : all) {
            actor.reset(new NodeNative[nodes]());
            for (std::uint32_t i = 0; i < nodes; ++i) {
                NodeNative* n = timing.time([&] { return &actor[i]; });
                n->payload = i;
                n->left = &actor[pick(i, 1)];
                n->right = &actor[pick(i, 7)];
            }
        }
        for (auto& actor : all) for (std::uint32_t i = 0; i < nodes; ++i) checksum += actor[i].payload + actor[i].left->payload;
        rss_built = rss_kib();
        for (auto& actor : all) actor.reset();
    } else if (variant == "offset32" || variant == "handle32") {
        const bool handles = variant == "handle32";
        // Realistic bounded per-actor heaps: reserve what the actor needs plus
        // slack, never a 4 GiB cage (matters for 100k actors on a 32-bit host).
        // Size from the real page geometry of the selected profile: 16-byte
        // nodes per slab page, plus the handle table and slack pages.
        constexpr std::size_t page = BenchHeap::kPageBytes;
        const std::size_t per_page = (page - BenchHeap::kHeaderBytes) / 16;
        const std::size_t table = handles ? (std::size_t{nodes} * 8 + BenchHeap::kHeaderBytes + page - 1) / page : 0;
        // Slack pages per actor (default 3). In a shared pool an actor costs about
        // min(slot, OS page) of RSS, so tiny actors need a tight reservation.
        const char* slack_env = std::getenv("ORES_BENCH_SLACK_PAGES");
        const std::size_t slack = slack_env ? std::strtoul(slack_env, nullptr, 10) : 3;
        const std::size_t reserve = ((nodes + per_page - 1) / per_page + table + slack) * page;
        BenchPool pool(actors, reserve); // one mapping for all actors
        if (!pool.ok()) { std::printf("%s %u %u %zu FAILED_RESERVE\n", variant.c_str(), actors, nodes, sizeof(void*) * 8); return 1; }
        std::vector<std::unique_ptr<BenchHeap>> heaps(actors);
        std::vector<void*> slots(actors);
        // Roots per actor: the guest would hold these as 4-byte fields.
        std::vector<std::vector<std::uint32_t>> roots(actors);
        for (std::uint32_t a = 0; a < actors; ++a) {
            slots[a] = pool.acquire();
            heaps[a] = std::make_unique<BenchHeap>(slots[a], pool.slot_bytes(),
                                                       handles ? nodes : 0u);
            BenchHeap& heap = *heaps[a];
            if (!heap.ok()) { std::printf("%s %u %u %zu FAILED_RESERVE\n", variant.c_str(), actors, nodes, sizeof(void*) * 8); return 1; }
            auto& root = roots[a];
            root.resize(nodes);
            for (std::uint32_t i = 0; i < nodes; ++i) {
                std::uint32_t word;
                NodeCompact* n;
                if (handles) {
                    auto h = timing.time([&] { return heap.allocate_handle(sizeof(NodeCompact)); });
                    if (!h) { std::printf("%s ALLOC_FAILED %d\n", variant.c_str(), int(h.status)); return 1; }
                    word = h.value.word;
                    n = static_cast<NodeCompact*>(heap.resolve(heap.resolve_handle(h.value).value, 16).value);
                } else {
                    auto r = timing.time([&] { return heap.allocate(sizeof(NodeCompact)); });
                    if (!r) { std::printf("%s ALLOC_FAILED %d\n", variant.c_str(), int(r.status)); return 1; }
                    word = r.value.storage();
                    n = static_cast<NodeCompact*>(heap.resolve(r.value, 16).value);
                }
                n->payload = i;
                n->left = root[pick(i, 1)];
                n->right = root[pick(i, 7)];
                root[i] = word;
            }
            metadata += heap.stats().metadata_bytes + root.capacity() * sizeof(std::uint32_t);
        }
        for (std::uint32_t a = 0; a < actors; ++a)
            for (std::uint32_t w : roots[a]) {
                const BenchHeap::Ref r = handles
                    ? heaps[a]->resolve_handle(BenchHeap::Handle32{w}).value
                    : BenchHeap::Ref::from_storage(w);
                checksum += static_cast<NodeCompact*>(heaps[a]->resolve(r, 16).value)->payload;
            }
        rss_built = rss_kib();
        for (std::uint32_t a = 0; a < actors; ++a) {          // actor STOP
            heaps[a]->retire();
            pool.release(slots[a]);
        }
        for (auto& r : roots) std::vector<std::uint32_t>().swap(r);
    } else {
        std::fprintf(stderr, "unknown variant %s\n", variant.c_str());
        return 2;
    }
    const std::size_t rss_after = rss_kib();
    const double total_nodes = double(actors) * nodes;
    std::printf("%s %u %u %zu %zu %zu %.1f %.1f %.0f %zu %lld\n", variant.c_str(), actors, nodes,
                sizeof(void*) * 8, rss_built - base_rss,
                rss_after > base_rss ? rss_after - base_rss : 0,
                double(rss_built - base_rss) * 1024.0 / total_nodes,
                timing.n ? timing.total_ns / double(timing.n) : 0.0, timing.max_ns,
                metadata / 1024, static_cast<long long>(checksum % 1000));
    return 0;
}
