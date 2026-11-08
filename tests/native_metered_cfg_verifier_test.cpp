#include "oreslang/backend/MeteredCfgVerifier.hpp"
#include <stdexcept>
#include <cstddef>
#include <iostream>
#include <vector>

using oreslang::backend_detail::every_cycle_contains_meter;
using Graph = std::vector<std::vector<std::size_t>>;

static void require(bool value) {
    if (!value) throw std::runtime_error("metered CFG verifier invariant failed");
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
