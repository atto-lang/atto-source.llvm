#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace oreslang::runtime::detail {

// Internal owner-confined graph admission primitive. All readers must describe
// one immutable snapshot under the arena mutation guard. Roots must be the
// complete strong-root set, not merely the caller's convenient references.
// Weak handles are not roots. This algorithm does not discover guest roots.
template<std::size_t Capacity>
struct ActorGraphSweepPlan final {
    static_assert(Capacity > 0, "empty graph capacity");
    std::array<bool, Capacity> reclaim{};
    std::size_t nodes = 0;
    std::size_t edges = 0;
};

// Preflight and plan only. No arena writes or guest callbacks. Each slot,
// root occurrence and adjacency occurrence consumes work, including repeated
// edges and unreachable nodes. A null plan indicates quota/cancellation;
// invalid indices or dangling edges fail closed with a controlled exception.
template<std::size_t Capacity, class Live, class EdgeCount, class EdgeAt, class RootAt>
std::unique_ptr<ActorGraphSweepPlan<Capacity>> plan_actor_graph_sweep(
        std::size_t root_count, std::size_t max_work, const std::atomic<bool>& cancelled,
        Live live, EdgeCount edge_count, EdgeAt edge_at, RootAt root_at) {
    if (root_count > Capacity || max_work < Capacity ||
            cancelled.load(std::memory_order_acquire)) return nullptr;
    struct Scratch {
        std::array<bool, Capacity> marked{};
        std::array<std::size_t, Capacity> queue{};
    };
    auto scratch = std::make_unique<Scratch>();
    auto plan = std::make_unique<ActorGraphSweepPlan<Capacity>>();
    auto charge = [&]() noexcept {
        if (!max_work || cancelled.load(std::memory_order_acquire)) return false;
        --max_work;
        return true;
    };
    auto require_live = [&](std::size_t id) {
        if (id >= Capacity || !live(id))
            throw std::logic_error("invalid graph sweep reference");
    };

    // Inspect ALL live edges before deciding reachability, including the
    // components which will be reclaimed. Stale edges cannot disappear into
    // garbage and masquerade as a valid graph.
    for (std::size_t i = 0; i < Capacity; ++i) {
        if (!charge()) return nullptr;
        if (!live(i)) continue;
        const auto count = edge_count(i);
        for (std::size_t e = 0; e < count; ++e) {
            if (!charge()) return nullptr;
            require_live(edge_at(i, e));
        }
    }
    std::size_t tail = 0;
    auto mark = [&](std::size_t id) {
        require_live(id);
        if (!scratch->marked[id]) {
            scratch->marked[id] = true;
            scratch->queue[tail++] = id;
        }
    };
    for (std::size_t i = 0; i < root_count; ++i) {
        if (!charge()) return nullptr;
        mark(root_at(i));
    }
    for (std::size_t cursor = 0; cursor < tail; ++cursor) {
        if (!charge()) return nullptr;
        const auto id = scratch->queue[cursor];
        const auto count = edge_count(id);
        for (std::size_t e = 0; e < count; ++e) {
            if (!charge()) return nullptr;
            mark(edge_at(id, e));
        }
    }
    for (std::size_t i = 0; i < Capacity; ++i) {
        if (!charge()) return nullptr;
        if (!live(i) || scratch->marked[i]) continue;
        plan->reclaim[i] = true;
        ++plan->nodes;
        const auto count = edge_count(i);
        // The preflight charged each of these edges already; this sum is
        // bounded by that successful preflight and cannot overflow size_t.
        plan->edges += count;
    }
    if (cancelled.load(std::memory_order_acquire)) return nullptr;
    return plan;
}

} // namespace oreslang::runtime::detail
