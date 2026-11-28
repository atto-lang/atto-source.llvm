#include "oreslang/backend/OwnershipDataflowV1.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("shadow V0: " #expr); } while (false)

using namespace oreslang;

struct Observed {
    std::size_t block;
    std::size_t operation;
    OwnershipV1OpKind kind;
    std::size_t owner;
};
static void record(void* context, std::size_t block, std::size_t operation,
                   OwnershipV1OpKind kind, std::size_t owner) {
    auto& events = *static_cast<std::vector<Observed>*>(context);
    events.push_back({block, operation, kind, owner});
}
static void deny(void*, std::size_t, std::size_t, OwnershipV1OpKind, std::size_t) {
    throw std::runtime_error("shadow consumer failure");
}
static OwnershipV1Graph source_graph() {
    OwnershipV1Graph graph;
    graph.owners = {"owned_resource"};
    graph.entry = 0;
    graph.blocks.resize(2);
    graph.blocks[0].operations = {
        {OwnershipV1OpKind::Create, 0},
        {OwnershipV1OpKind::Use, 0},
    };
    graph.blocks[0].successors = {1};
    graph.blocks[1].operations = {
        {OwnershipV1OpKind::Use, 0},
        {OwnershipV1OpKind::ExitNormal, 0},
    };
    return graph;
}
int main() {
    const auto graph = source_graph();
    // The normal compiler path remains unchanged; it does not construct events.
    const auto plain = OwnershipDataflowV1::analyze(graph);
    REQUIRE(plain.normal_exits.size() == 1);
    REQUIRE(plain.normal_exits[0].cleanup_reverse_order == std::vector<std::size_t>{0});
    std::vector<Observed> events;
    const auto traced = OwnershipDataflowV1::analyze(graph, &record, &events);
    REQUIRE(traced.normal_exits.size() == plain.normal_exits.size());
    REQUIRE(traced.normal_exits[0].cleanup_reverse_order ==
           plain.normal_exits[0].cleanup_reverse_order);
    REQUIRE(events.size() == 4);
    REQUIRE(events[0].block == 0 && events[0].operation == 0 &&
           events[0].kind == OwnershipV1OpKind::Create && events[0].owner == 0);
    REQUIRE(events[1].kind == OwnershipV1OpKind::Use);
    REQUIRE(events[2].block == 1 && events[2].kind == OwnershipV1OpKind::Use);
    REQUIRE(events[3].kind == OwnershipV1OpKind::ExitNormal);

    // Rejection cannot generate an apparent accepted operation at the failed step.
    auto invalid = source_graph();
    invalid.blocks[0].operations = {{OwnershipV1OpKind::Use, 0}};
    events.clear();
    bool rejected = false;
    try { (void)OwnershipDataflowV1::analyze(invalid, &record, &events); }
    catch (const std::runtime_error&) { rejected = true; }
    REQUIRE(rejected && events.empty());

    // A later invalid block must not publish prior locally accepted events.
    auto late = source_graph();
    late.blocks[1].operations = {
        {OwnershipV1OpKind::Drop, 0},
        {OwnershipV1OpKind::Use, 0},
        {OwnershipV1OpKind::ExitNormal, 0},
    };
    events.clear();
    rejected = false;
    try { (void)OwnershipDataflowV1::analyze(late, &record, &events); }
    catch (const std::runtime_error&) { rejected = true; }
    REQUIRE(rejected && events.empty());

    // A valid 66 x 1024-operation CFG exceeds the opt-in trace budget,
    // while preserving normal, untraced compiler analysis behavior.
    OwnershipV1Graph large;
    large.entry = 0;
    large.owners = {"owned_resource"};
    large.blocks.resize(66);
    for (std::size_t block = 0; block < large.blocks.size(); ++block) {
        auto& current = large.blocks[block];
        current.operations.assign(1024, {OwnershipV1OpKind::Use, 0});
        if (block == 0)
            current.operations[0] = {OwnershipV1OpKind::Create, 0};
        if (block + 1 < large.blocks.size())
            current.successors = {block + 1};
        else
            current.operations.back() = {OwnershipV1OpKind::ExitNormal, 0};
    }
    REQUIRE(OwnershipDataflowV1::analyze(large).normal_exits.size() == 1);
    events.clear();
    rejected = false;
    try { (void)OwnershipDataflowV1::analyze(large, &record, &events); }
    catch (const std::runtime_error&) { rejected = true; }
    REQUIRE(rejected && events.empty());

    // Exactly the budget is allowed, and all events are published.
    auto atLimit = large;
    atLimit.blocks.resize(64);
    atLimit.blocks.back().successors.clear();
    atLimit.blocks.back().operations.back() = {OwnershipV1OpKind::ExitNormal, 0};
    events.clear();
    REQUIRE(OwnershipDataflowV1::analyze(atLimit, &record, &events).normal_exits.size() == 1);
    REQUIRE(events.size() == OwnershipDataflowV1::MaxShadowEventsV0);

    // A reachable exit does not validate a second path that cycles.
    auto cycle = source_graph();
    cycle.blocks.resize(3);
    cycle.blocks[0].successors = {1, 2};
    cycle.blocks[2].operations = {{OwnershipV1OpKind::Use, 0}};
    cycle.blocks[2].successors = {2};
    events.clear();
    rejected = false;
    try { (void)OwnershipDataflowV1::analyze(cycle, &record, &events); }
    catch (const std::runtime_error&) { rejected = true; }
    REQUIRE(rejected && events.empty());

    // A failing consumer must abort verification, never grant ownership.
    rejected = false;
    try { (void)OwnershipDataflowV1::analyze(graph, &deny, nullptr); }
    catch (const std::runtime_error&) { rejected = true; }
    REQUIRE(rejected);
    return 0;
}
