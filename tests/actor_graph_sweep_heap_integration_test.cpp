#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedGraphHeap;
static std::size_t checks = 0;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F>
static void denied(F action, const char* message) {
    bool refused = false;
    try { action(); } catch (const std::logic_error&) { refused = true; }
    check(refused, message);
}

struct Resource {
    int* drops;
    explicit Resource(int* count) noexcept : drops(count) {}
    ~Resource() noexcept { ++*drops; }
};

int main() {
    using Heap = ActorOwnedGraphHeap<8, 32, 3>;
    const ActorHeapIdentity owner{1, 2, 3}, foreign{1, 2, 4};
    std::atomic<bool> cancelled{false};
    std::uint64_t one = 1, two = 2, three = 3, four = 4, out = 0;
    Heap heap(owner);
    const auto a = heap.allocate(owner, &one, sizeof(one)).value();
    const auto b = heap.allocate(owner, &two, sizeof(two)).value();
    const auto c = heap.allocate(owner, &three, sizeof(three)).value();
    const auto d = heap.allocate(owner, &four, sizeof(four)).value();
    check(heap.link(owner, a, b) && heap.link(owner, a, c) &&
          heap.link(owner, b, d) && heap.link(owner, c, d) &&
          heap.link(owner, d, a) && heap.link(owner, d, d), "fixture links");
    check(!heap.clone_trusted_pod_graph(owner, a, 3, 6, cancelled), "clone node budget");
    check(!heap.clone_trusted_pod_graph(owner, a, 4, 5, cancelled), "clone edge budget");
    cancelled.store(true);
    check(!heap.clone_trusted_pod_graph(owner, a, 4, 6, cancelled), "clone cancellation");
    cancelled.store(false);
    auto loan = heap.borrow(owner);
    check(bool(loan), "read loan");
    denied([&] { (void)heap.clone_trusted_pod_graph(owner, a, 4, 6, cancelled); }, "clone while borrowed");
    loan->reset();
    check(heap.live_nodes(owner) == 4 && heap.live_edges(owner) == 6, "clone refusal ledger");
    const auto aa = heap.clone_trusted_pod_graph(owner, a, 4, 6, cancelled).value();
    const auto bb = heap.edge_at(owner, aa, 0).value();
    const auto cc = heap.edge_at(owner, aa, 1).value();
    const auto dd = heap.edge_at(owner, bb, 0).value();
    check(!(aa == a) && !(bb == b) && !(cc == c) && !(dd == d), "fresh clone identities");
    check(heap.edge_at(owner, cc, 0).value() == dd &&
          heap.edge_at(owner, dd, 0).value() == aa &&
          heap.edge_at(owner, dd, 1).value() == dd, "shared child and circular edges");
    check(heap.live_nodes(owner) == 8 && heap.live_edges(owner) == 12, "clone ledger");
    check(heap.copy_out(owner, dd, &out, sizeof(out)) == sizeof(out) && out == four, "cloned payload");
    const std::uint64_t changed = 99;
    heap.write(owner, dd, &changed, sizeof(changed));
    heap.copy_out(owner, d, &out, sizeof(out));
    check(out == four, "independent cloned payload");
    check(!heap.clone_trusted_pod_graph(owner, aa, 4, 6, cancelled), "full heap clone refusal");
    std::array<Heap::Handle, 2> roots{a, a};
    // Commit 8 + preflight 8 slots/12 edges + 2 roots + mark 4 nodes/6 edges + classify 8 = 48.
    check(!heap.reclaim_unreachable_trusted_pod_graph(owner, roots.data(), roots.size(), 47, cancelled), "sweep exact work minus one");
    cancelled.store(true);
    check(!heap.reclaim_unreachable_trusted_pod_graph(owner, roots.data(), roots.size(), 48, cancelled), "sweep cancellation");
    cancelled.store(false);
    loan = heap.borrow(owner);
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(owner, roots.data(), roots.size(), 48, cancelled); }, "sweep while borrowed");
    loan->reset();
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(foreign, roots.data(), roots.size(), 48, cancelled); }, "foreign actor root sweep");
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(owner, nullptr, 1, 48, cancelled); }, "missing roots");
    check(heap.live_nodes(owner) == 8 && heap.live_edges(owner) == 12, "refused sweep ledger");
    check(heap.reclaim_unreachable_trusted_pod_graph(owner, roots.data(), roots.size(), 48, cancelled).value() == 4, "exact work sweep");
    check(heap.live_nodes(owner) == 4 && heap.live_edges(owner) == 6, "survivor ledger");
    check(heap.edge_at(owner, b, 0).value() == d && heap.edge_at(owner, c, 0).value() == d &&
          heap.edge_at(owner, d, 0).value() == a, "survivor topology");
    denied([&] { (void)heap.copy_out(owner, aa, &out, sizeof(out)); }, "stale clone handle");
    const auto replacement = heap.allocate(owner, &one, sizeof(one)).value();
    check(replacement.slot == aa.slot && replacement.generation > aa.generation, "swept slot generation");
    std::array<Heap::Handle, 1> stale{aa};
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(owner, stale.data(), stale.size(), 100, cancelled); }, "stale root refusal");
    Heap other(owner);
    const auto other_root = other.allocate(owner, &one, sizeof(one)).value();
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(owner, &other_root, 1, 100, cancelled); }, "foreign heap root refusal");
    check(heap.live_nodes(owner) == 5 && heap.live_edges(owner) == 6, "invalid roots no mutation");
    check(heap.reclaim_unreachable_trusted_pod_graph(owner, nullptr, 0, 100, cancelled).value() == 5, "empty root set");
    check(heap.live_nodes(owner) == 0 && heap.live_edges(owner) == 0, "all reclaimed");
    int drops = 0;
    const auto resource = heap.emplace_owned<Resource>(owner, &drops).value();
    const auto root = heap.allocate(owner, &one, sizeof(one)).value();
    check(heap.link(owner, root, resource), "nested owned fixture");
    denied([&] { (void)heap.clone_trusted_pod_graph(owner, root, 2, 1, cancelled); }, "nested owned clone refused");
    denied([&] { (void)heap.reclaim_unreachable_trusted_pod_graph(owner, nullptr, 0, 100, cancelled); }, "owned sweep refused");
    check(drops == 0 && heap.live_nodes(owner) == 2 && heap.live_edges(owner) == 1, "owned resource unchanged");
    heap.remove(owner, resource);
    check(drops == 1 && heap.live_edges(owner) == 0, "exactly-once resource drop");
    check(heap.reclaim_unreachable_trusted_pod_graph(owner, nullptr, 0, 100, cancelled).value() == 1, "sweep retry");

    // Independent fixed-point reachability oracle against the ACTUAL heap,
    // not a mock arena. Each iteration also tests stale handles after reuse.
    std::mt19937 rng(20261125);
    using RandomHeap = ActorOwnedGraphHeap<32, 16, 4>;
    for (int trial = 0; trial < 1000; ++trial) {
        RandomHeap h(owner);
        std::array<RandomHeap::Handle, 32> ids{};
        std::array<std::vector<std::size_t>, 32> edges{};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::uint64_t payload = i;
            ids[i] = h.allocate(owner, &payload, sizeof(payload)).value();
        }
        for (std::size_t i = 0; i < ids.size(); ++i) {
            for (unsigned e = 0; e < 4; ++e) {
                const std::size_t target = rng() % ids.size();
                if ((rng() & 1U) && h.link(owner, ids[i], ids[target]))
                    edges[i].push_back(target);
            }
        }
        std::vector<RandomHeap::Handle> random_roots;
        std::array<bool, 32> marked{};
        for (unsigned i = 0, count = rng() % 6; i < count; ++i) {
            const std::size_t target = rng() % ids.size();
            random_roots.push_back(ids[target]);
            marked[target] = true;
        }
        bool progress;
        do {
            progress = false;
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (marked[i]) for (auto target : edges[i]) if (!marked[target]) {
                    marked[target] = true;
                    progress = true;
                }
        } while (progress);
        std::size_t live_nodes = 0, live_edges = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) if (marked[i]) {
            ++live_nodes;
            live_edges += edges[i].size();
        }
        check(h.reclaim_unreachable_trusted_pod_graph(owner, random_roots.data(),
                random_roots.size(), 1000, cancelled).value() == ids.size() - live_nodes, "oracle reclaimed count");
        check(h.live_nodes(owner) == live_nodes && h.live_edges(owner) == live_edges, "oracle live ledger");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (marked[i]) {
                h.copy_out(owner, ids[i], &out, sizeof(out));
                check(out == i, "oracle surviving payload");
                for (std::size_t e = 0; e < edges[i].size(); ++e)
                    check(h.edge_at(owner, ids[i], e).value() == ids[edges[i][e]], "oracle surviving edge");
                check(!h.edge_at(owner, ids[i], edges[i].size()), "oracle adjacency length");
            } else {
                denied([&] { (void)h.copy_out(owner, ids[i], &out, sizeof(out)); }, "oracle stale slot");
                const auto fresh = h.allocate(owner, &one, sizeof(one)).value();
                check(fresh.slot == ids[i].slot && fresh.generation > ids[i].generation, "oracle recycled slot");
                denied([&] { (void)h.copy_out(owner, ids[i], &out, sizeof(out)); }, "oracle stale after reuse");
            }
        }
        check(h.live_nodes(owner) == ids.size() && h.live_edges(owner) == live_edges, "oracle reuse ledger");
    }
    std::cout << "PASS " << checks << " actual heap assertions (1000 independent randomized graph trials)\n";
}
