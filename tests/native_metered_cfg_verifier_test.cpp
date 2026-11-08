#include "oreslang/backend/MeteredCfgVerifier.hpp"
#include <stdexcept>
#include <cstddef>
#include <deque>
#include <iostream>
#include <vector>

using oreslang::backend_detail::every_cycle_contains_meter;
using Graph = std::vector<std::vector<std::size_t>>;

static void require(bool value) {
    if (!value) throw std::runtime_error("metered CFG verifier invariant failed");
}

// Independent oracle: Kahn topological elimination after deleting all
// metered blocks. This cross-checks the production DFS against exhaustive
// small graph possibilities rather than repeating its traversal algorithm.
static bool independent_oracle(const Graph& graph,
                               const std::vector<std::size_t>& marked) {
    const std::size_t n = graph.size();
    if (n == 0 || marked.empty()) return false;
    std::vector<bool> removed(n, false);
    for (const auto node : marked) {
        if (node >= n) return false;
        removed[node] = true;
    }
    std::vector<std::size_t> incoming(n, 0);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (removed[i]) continue;
        ++expected;
        for (std::size_t j : graph[i]) {
            if (j >= n) return false;
            if (!removed[j]) ++incoming[j];
        }
    }
    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (!removed[i] && incoming[i] == 0) ready.push_back(i);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto i = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto j : graph[i])
            if (!removed[j] && --incoming[j] == 0) ready.push_back(j);
    }
    return visited == expected;
}

int main() {
    // Source scalar loop: every cycle includes a committed-fuel debit.
    require(every_cycle_contains_meter(Graph{{1}, {2, 3}, {0}, {}}, {2}));
    // Unmetered backedge, even when the same graph contains a meter.
    require(!every_cycle_contains_meter(Graph{{1, 2}, {0}, {3}, {}}, {2}));
    // Backedges in otherwise unreachable code are rejected fail-closed.
    require(!every_cycle_contains_meter(Graph{{1}, {}, {3}, {2}}, {1}));
    // A self-loop on an explicitly metered block is permitted.
    require(every_cycle_contains_meter(Graph{{0}}, {0}));
    // An unmetered self-loop is never permitted.
    require(!every_cycle_contains_meter(Graph{{1}, {1}, {0}}, {0}));
    // Two separate cycles must each intersect a metered block.
    require(!every_cycle_contains_meter(Graph{{1}, {0, 2}, {3}, {2}}, {1}));
    require(every_cycle_contains_meter(Graph{{1}, {0, 2}, {3}, {2}}, {1, 3}));
    // Reject malformed IDs, edges, missing markers, empty graphs.
    require(!every_cycle_contains_meter(Graph{}, {0}));
    require(!every_cycle_contains_meter(Graph{{1}, {}}, {2}));
    require(!every_cycle_contains_meter(Graph{{2}, {}}, {0}));
    require(!every_cycle_contains_meter(Graph{{1}, {0}}, {}));
    // Exhaustively enumerate all 512 directed 3-node graphs and their
    // 7 nonempty subsets of trusted metering nodes.
    for (unsigned edge_mask = 0; edge_mask < (1u << 9); ++edge_mask) {
        Graph graph(3);
        for (unsigned src = 0; src < 3; ++src)
            for (unsigned dst = 0; dst < 3; ++dst)
                if ((edge_mask & (1u << (src * 3 + dst))) != 0)
                    graph[src].push_back(dst);
        for (unsigned node_mask = 1; node_mask < 8; ++node_mask) {
            std::vector<std::size_t> markers;
            for (unsigned node = 0; node < 3; ++node)
                if ((node_mask & (1u << node)) != 0) markers.push_back(node);
            require(every_cycle_contains_meter(graph, markers) ==
                    independent_oracle(graph, markers));
        }
    }
    // Reject invalid successors even if that block is marked metered.
    require(!every_cycle_contains_meter(Graph{{}, {2}}, {1}));
    // Bound CPU and memory even for otherwise valid giant CFG inputs.
    Graph too_many_blocks(oreslang::backend_detail::kMaxMeteredCfgNodes + 1);
    require(!every_cycle_contains_meter(too_many_blocks, {0}));
    Graph too_many_edges(1);
    too_many_edges[0].assign(oreslang::backend_detail::kMaxMeteredCfgEdges + 1, 0);
    require(!every_cycle_contains_meter(too_many_edges, {0}));
    // Accommodate deep CFGs without recursive verifier-stack exhaustion.
    constexpr std::size_t N = 200000;
    Graph deep(N);
    for (std::size_t i = 0; i + 1 < N; ++i) deep[i].push_back(i + 1);
    deep[N - 1].push_back(0);
    require(every_cycle_contains_meter(deep, {N / 2}));
    deep[4].push_back(4); // new unmetered self-loop bypasses the marker
    require(!every_cycle_contains_meter(deep, {N / 2}));
    std::cout << "METERED_CFG_CYCLE_COVERAGE_PASS\n";
}
