#pragma once
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang {

// Experimental native subset only. Does not prove termination, ownership,
// capability security, resource budgets, callback timing, or source parity.
struct NativeStaticSafetyFinding {
    std::string rule;
    std::string confidence = "potential";
    std::string callable;
    std::string message;
    std::vector<std::string> component;
};

namespace native_static_detail {

struct Arc { std::string from, to, kind; };
struct Frame { std::string node; std::set<std::string>::const_iterator next; };

inline std::vector<std::vector<std::string>> strongly_connected(
        const std::map<std::string, std::set<std::string>>& forward,
        const std::map<std::string, std::set<std::string>>& reverse) {
    std::set<std::string> seen;
    std::vector<std::string> order;
    for (const auto& item : forward) {
        if (!seen.insert(item.first).second) continue;
        std::vector<Frame> stack;
        stack.push_back({item.first, item.second.begin()});
        while (!stack.empty()) {
            auto& top = stack.back();
            const auto& children = forward.at(top.node);
            if (top.next == children.end()) {
                order.push_back(top.node);
                stack.pop_back();
                continue;
            }
            const std::string neighbor = *top.next++;
            if (seen.insert(neighbor).second)
                stack.push_back({neighbor, forward.at(neighbor).begin()});
        }
    }
    seen.clear();
    std::vector<std::vector<std::string>> components;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!seen.insert(*it).second) continue;
        std::vector<std::string> stack{*it};
        std::vector<std::string> component;
        while (!stack.empty()) {
            auto node = std::move(stack.back());
            stack.pop_back();
            component.push_back(node);
            for (const auto& parent : reverse.at(node))
                if (seen.insert(parent).second) stack.push_back(parent);
        }
        std::sort(component.begin(), component.end());
        components.push_back(std::move(component));
    }
    return components;
}

} // namespace native_static_detail

inline std::vector<NativeStaticSafetyFinding>
analyze_native_static_safety(const Program& program) {
    if (program.functions.size() + program.actors.size() > 4096)
        throw std::runtime_error("native static safety callable budget exceeded");

    std::map<std::string, const Function*> callables;
    std::map<std::string, std::string> actors;
    for (const auto& f : program.functions) {
        std::string id = realtime_detail::function_key(f.name, f.parameters.size());
        if (!callables.emplace(id, &f).second)
            throw std::runtime_error("ambiguous overload in native static safety analysis");
    }
    for (const auto& actor : program.actors) {
        std::string id = "actor:" + actor.name + ".on_message";
        if (!callables.emplace(id, &actor.on_message).second)
            throw std::runtime_error("duplicate native actor receiver");
        actors.emplace(actor.name, id);
    }

    std::vector<NativeStaticSafetyFinding> findings;
    for (const auto& loop : program.cooperative_loops)
        findings.push_back({"ORES-TERM-002", "potential", loop.name,
            "cooperative source loop lacks a verified finite-iteration bound", {}});
    // ScalarWhileLoop has a literal upper bound and monotone +1 lowering,
    // unlike an arbitrary while; no infinite-loop warning for that subset.

    std::vector<native_static_detail::Arc> arcs;
    for (const auto& entry : callables) {
        const auto& id = entry.first;
        const Function& f = *entry.second;
        // The shared scanner may stop at its budget; never analyze a partial
        // call graph as if it were complete.
        const auto direct = realtime_detail::direct_facts(f, 30000 - arcs.size());
        if (direct.call_budget_exceeded)
            throw std::runtime_error("native static safety graph budget exceeded");
        for (const auto& callee : direct.calls) {
            if (callables.count(callee)) arcs.push_back({id, callee, "call"});
        }
        std::map<std::string, std::string> spawned_refs;
        for (const auto& spawn : f.actor_spawns) {
            if (!spawned_refs.emplace(spawn.name, spawn.actor_type).second)
                throw std::runtime_error("ambiguous native actor spawn binding");
        }
        // Spawn is NOT handler invocation. A message sent to a proven
        // reference created by this function creates a potential delivery edge.
        for (const auto& send : f.actor_sends) {
            auto binding = spawned_refs.find(send.reference);
            if (binding == spawned_refs.end()) {
                findings.push_back({"ORES-ANALYSIS-001", "unknown", id,
                    "actor message receiver is not a locally resolved spawn; delivery graph incomplete", {}});
                continue;
            }
            auto destination = actors.find(binding->second);
            if (destination == actors.end()) {
                findings.push_back({"ORES-ANALYSIS-001", "unknown", id,
                    "spawned actor message handler unresolved", {}});
                continue;
            }
            if (arcs.size() >= 30000)
                throw std::runtime_error("native static safety graph budget exceeded");
            arcs.push_back({id, destination->second, "actor_delivery_after_spawn"});
        }
    }
    if (arcs.size() > 30000) throw std::runtime_error("native static safety graph budget exceeded");

    std::map<std::string, std::set<std::string>> forward, reverse;
    for (const auto& entry : callables) {
        forward.emplace(entry.first, std::set<std::string>{});
        reverse.emplace(entry.first, std::set<std::string>{});
    }
    for (const auto& arc : arcs) {
        forward.at(arc.from).insert(arc.to);
        reverse.at(arc.to).insert(arc.from);
    }
    for (const auto& component : native_static_detail::strongly_connected(forward, reverse)) {
        std::set<std::string> members(component.begin(), component.end());
        std::vector<native_static_detail::Arc> inside;
        for (const auto& arc : arcs)
            if (members.count(arc.from) && members.count(arc.to))
                inside.push_back(arc);
        if (inside.empty()) continue;
        if (component.size() == 1
                && std::none_of(inside.begin(), inside.end(), [&](const auto& arc) {
                    return arc.from == component[0] && arc.to == component[0]; }))
            continue;
        const bool actor = std::any_of(inside.begin(), inside.end(), [](const auto& arc) {
            return arc.kind == "actor_delivery_after_spawn";
        });
        findings.push_back({actor ? "ORES-AMP-001" : "ORES-AMP-004",
            "potential", component[0],
            actor ? "recursive actor delivery after spawn may amplify work; no total bound verified"
                  : "recursive call cycle has no decreasing measure proven",
            component});
    }
    std::sort(findings.begin(), findings.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.rule != rhs.rule) return lhs.rule < rhs.rule;
        if (lhs.callable != rhs.callable) return lhs.callable < rhs.callable;
        return lhs.message < rhs.message;
    });
    return findings;
}

} // namespace oreslang
