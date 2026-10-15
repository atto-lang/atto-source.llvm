#include "oreslang/runtime/NeuralMoERoutingPlan.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Release/NDEBUG must execute the same security invariants.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using Plan = oreslang::runtime::NeuralMoERoutingPlan;
using Overflow = Plan::OverflowPolicy;

template <typename Error, typename Fn>
void must_fail(Fn&& action) {
    bool failed = false;
    try { action(); }
    catch (const Error&) { failed = true; }
    assert(failed);
}

int main() {
    using Token = Plan::TokenScores;
    const std::vector<Token> one{{5, {{0, 3}, {1, 2}, {2, 1}}}};
    auto basic = Plan::plan(one, 3, 2, 3, Overflow::reject);
    assert(basic.assignments.size() == 2);
    assert(basic.assignments[0].expert_index == 0);
    assert(basic.assignments[1].expert_index == 1);
    assert(std::fabs(basic.assignments[0].normalized_weight - .6) < 1e-12);
    assert(std::fabs(basic.assignments[1].normalized_weight - .4) < 1e-12);
    assert((basic.expert_loads == std::vector<std::size_t>{1, 1, 0}));

    // Tie-break is expert index, not source input order.
    auto ties = Plan::plan({{0, {{2, 5}, {1, 5}, {0, 5}}}},
                            3, 2, 1, Overflow::reject);
    assert(ties.assignments[0].expert_index == 0);
    assert(ties.assignments[1].expert_index == 1);
    assert(std::fabs(ties.assignments[0].normalized_weight - .5) < 1e-12);

    const std::vector<Token> overflowing{
        {0, {{0, 7}, {1, 6}, {2, 1}}},
        {1, {{0, 8}, {1, 7}, {2, 1}}}
    };
    auto dropped = Plan::plan(overflowing, 3, 2, 1, Overflow::drop_token);
    assert(dropped.assignments.size() == 2);
    assert((dropped.dropped_token_indices == std::vector<std::uint64_t>{1}));
    assert((dropped.expert_loads == std::vector<std::size_t>{1, 1, 0}));
    must_fail<std::overflow_error>([&] {
        Plan::plan(overflowing, 3, 2, 1, Overflow::reject);
    });

    const double maximum = std::numeric_limits<double>::max();
    auto huge = Plan::plan({{0, {{0, maximum}, {1, maximum}, {2, maximum / 2}}}},
                            3, 2, 1, Overflow::reject);
    assert(std::fabs(huge.assignments[0].normalized_weight - .5) < 1e-12);
    assert(std::fabs(huge.assignments[1].normalized_weight - .5) < 1e-12);

    must_fail<std::invalid_argument>([] {
        Plan::plan({{0, {{0, 0}, {1, 0}, {2, 0}}}}, 3, 2, 1, Overflow::reject);
    });
    must_fail<std::invalid_argument>([] {
        Plan::plan({{0, {{0, 1}, {1, 2}, {2, 3}}},
                    {0, {{0, 4}, {1, 5}, {2, 6}}}}, 3, 2, 2, Overflow::reject);
    });
    must_fail<std::invalid_argument>([] {
        Plan::plan({{0, {{1, 1}, {1, 2}, {2, 3}}}}, 3, 2, 2, Overflow::reject);
    });
    must_fail<std::invalid_argument>([] {
        Plan::plan({{0, {{0, std::numeric_limits<double>::quiet_NaN()},
                          {1, 2}, {2, 3}}}}, 3, 2, 2, Overflow::reject);
    });
    must_fail<std::invalid_argument>([&] {
        Plan::plan(one, 3, 4, 1, Overflow::reject);
    });
    must_fail<std::invalid_argument>([] {
        Plan::plan({}, 3, 2, 1, Overflow::reject);
    });
    return 0;
}
