#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace oreslang::backend_detail {

// Conservatively bound attacker-influenced compiler analysis memory and time.
// These ceilings are much higher than the experimental scalar guest CFG.
inline constexpr std::size_t kMaxMeteredCfgNodes = 262144;
inline constexpr std::size_t kMaxMeteredCfgEdges = 1048576;

// Fail closed when a CFG has any cycle that can avoid the supplied, trusted
// metering blocks. Input indices are intentionally plain integers, so this
// invariant can be tested independently of an LLVM installation. This checks
// control-flow coverage, not the semantics of a metering instruction.
inline bool every_cycle_contains_meter(
        const std::vector<std::vector<std::size_t>>& successors,
        const std::vector<std::size_t>& metered_nodes) {
    const std::size_t count = successors.size();
    if (count == 0 || count > kMaxMeteredCfgNodes || metered_nodes.empty())
        return false;
    std::vector<unsigned char> metered(count, 0);
    for (const std::size_t id : metered_nodes) {
        if (id >= count) return false;
        metered[id] = 1;
    }
    // Reject malformed or excessively large CFGs even when their edges are
    // unreachable or their source blocks are already marked metered.
    std::size_t edge_count = 0;
    for (const auto& outgoing : successors) {
        if (outgoing.size() > kMaxMeteredCfgEdges - edge_count) return false;
        edge_count += outgoing.size();
        for (const std::size_t next : outgoing)
            if (next >= count) return false;
    }

    // A cycle that remains after removing all trusted metering blocks is a
    // potential unmetered infinite execution path. Use iterative DFS rather
    // than unbounded native recursion on an attacker-influenced CFG.
    std::vector<unsigned char> color(count, 0);
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    for (std::size_t start = 0; start < count; ++start) {
        if (metered[start] || color[start] != 0) continue;
        color[start] = 1;
        stack.emplace_back(start, 0);
        while (!stack.empty()) {
            auto& top = stack.back();
            const auto& edges = successors[top.first];
            if (top.second == edges.size()) {
                color[top.first] = 2;
                stack.pop_back();
                continue;
            }
            const std::size_t next = edges[top.second++];
            if (metered[next]) continue;
            if (color[next] == 1) return false;
            if (color[next] == 0) {
                color[next] = 1;
                stack.emplace_back(next, 0);
            }
        }
    }
    return true;
}

} // namespace oreslang::backend_detail
