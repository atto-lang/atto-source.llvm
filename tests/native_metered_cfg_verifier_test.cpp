#include "oreslang/backend/MeteredCfgVerifier.hpp"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <vector>

using oreslang::backend_detail::every_cycle_contains_meter;
using Graph = std::vector<std::vector<std::size_t>>;

int main() {
    // Source scalar loop: every cycle includes a committed-fuel debit.
    assert(every_cycle_contains_meter(Graph{{1}, {2, 3}, {0}, {}}, {2}));
    // Unmetered backedge, even when the same graph contains a meter.
    assert(!every_cycle_contains_meter(Graph{{1, 2}, {0}, {3}, {}}, {2}));
    // Backedges in otherwise unreachable code are rejected fail-closed.
    assert(!every_cycle_contains_meter(Graph{{1}, {}, {3}, {2}}, {1}));
    // A self-loop on an explicitly metered block is permitted.
    assert(every_cycle_contains_meter(Graph{{0}}, {0}));
    // An unmetered self-loop is never permitted.
    assert(!every_cycle_contains_meter(Graph{{1}, {1}, {0}}, {0}));
    // Two separate cycles must each intersect a metered block.
    assert(!every_cycle_contains_meter(Graph{{1}, {0, 2}, {3}, {2}}, {1}));
    assert(every_cycle_contains_meter(Graph{{1}, {0, 2}, {3}, {2}}, {1, 3}));
    // Reject malformed IDs, edges, missing markers, empty graphs.
    assert(!every_cycle_contains_meter(Graph{}, {0}));
    assert(!every_cycle_contains_meter(Graph{{1}, {}}, {2}));
    assert(!every_cycle_contains_meter(Graph{{2}, {}}, {0}));
    assert(!every_cycle_contains_meter(Graph{{1}, {0}}, {}));
    // Accommodate deep CFGs without recursive verifier-stack exhaustion.
    constexpr std::size_t N = 200000;
    Graph deep(N);
    for (std::size_t i = 0; i + 1 < N; ++i) deep[i].push_back(i + 1);
    deep[N - 1].push_back(0);
    assert(every_cycle_contains_meter(deep, {N / 2}));
    deep[4].push_back(4); // new unmetered self-loop bypasses the marker
    assert(!every_cycle_contains_meter(deep, {N / 2}));
    std::cout << "METERED_CFG_CYCLE_COVERAGE_PASS\n";
}
