#pragma once

#include "oreslang/types/RealtimeAnalysis.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace oreslang {

/**
 * Temporary native fail-closed admission until compiler-managed bounded
 * guest frames exist. An OS carrier stack is NOT an actor's allocation heap.
 *
 * Unlike Java's M0 runtime guard, this gate prevents the current native LLVM
 * direct-call lowering from emitting an unbounded recursive cycle at all.
 * It does not turn StackOverflow/SIGSEGV into a recoverable actor failure.
 * Never replace rejection with llvm.trap/abort/longjmp on a shared carrier.
 *
 * All source callable identities use name/arity; no host stack pointer,
 * actor reference, source text or user-controlled path is retained here.
 */
class NativeGuestCallGraphGate final {
public:
    static constexpr std::size_t max_functions = 4096;
    static constexpr std::size_t max_edges = 16384;
    static constexpr std::size_t max_depth = 64;
    using Graph = std::unordered_map<std::string, std::vector<std::string>>;

    static std::size_t require_bounded(const Graph& graph) {
        if (graph.size() > max_functions)
            throw std::runtime_error("native guest call graph function budget exceeded");
        std::size_t total_edges = 0;
        for (const auto& entry : graph) {
            if (entry.first.empty())
                throw std::runtime_error("native guest call graph has invalid function identity");
            if (entry.second.size() > max_edges - total_edges)
                throw std::runtime_error("native guest call graph edge budget exceeded");
            total_edges += entry.second.size();
            for (const auto& child : entry.second) {
                if (child.empty() || graph.find(child) == graph.end())
                    throw std::runtime_error("native guest call graph has unresolved target");
            }
        }

        // A cached longest path is valid regardless of its caller; each parent
        // checks its own total depth before admitting the graph.
        std::unordered_map<std::string, std::uint8_t> state;
        std::unordered_map<std::string, std::size_t> longest;
        std::function<std::size_t(const std::string&, std::size_t)> visit =
                [&](const std::string& key, std::size_t depth) -> std::size_t {
            if (depth > max_depth)
                throw std::runtime_error("native guest call depth budget exceeded");
            const auto status = state.find(key);
            if (status != state.end()) {
                if (status->second == 1)
                    throw std::runtime_error("native recursive call graph requires bounded guest frames");
                if (status->second == 2) return longest.at(key);
            }
            state[key] = 1;
            std::size_t span = 1;
            for (const auto& child : graph.at(key)) {
                const auto child_span = visit(child, depth + 1);
                if (child_span >= max_depth)
                    throw std::runtime_error("native guest call depth budget exceeded");
                span = std::max(span, child_span + 1);
            }
            state[key] = 2;
            longest.emplace(key, span);
            return span;
        };
        std::size_t peak = 0;
        // All functions, not just main: host-callable public entries and
        // unused callbacks cannot bypass this conservative safety gate.
        for (const auto& entry : graph)
            peak = std::max(peak, visit(entry.first, 1));
        return peak;
    }
};

/**
 * Scan all direct expression call edges BEFORE tree shaking or LLVM emission.
 * Reuse the native real-time AST walker so conditional arms, nested locals,
 * deferred cleanup and constructor default expressions are not skipped.
 */
inline void require_native_guest_call_graph_bounded(const Program& source) {
    NativeGuestCallGraphGate::Graph graph;
    std::size_t edges = 0;
    auto add = [&](const std::string& identity,
                   realtime_detail::DirectFacts facts) {
        if (facts.call_budget_exceeded ||
                facts.calls.size() > NativeGuestCallGraphGate::max_edges - edges)
            throw std::runtime_error("native guest call graph edge budget exceeded");
        edges += facts.calls.size();
        if (!graph.emplace(identity, std::move(facts.calls)).second)
            throw std::runtime_error("native guest callable identity collision");
        if (graph.size() > NativeGuestCallGraphGate::max_functions)
            throw std::runtime_error("native guest call graph function budget exceeded");
    };
    auto remaining = [&]() {
        return NativeGuestCallGraphGate::max_edges - edges;
    };
    for (const auto& function : source.functions) {
        add(realtime_detail::function_key(
                    function.name, function.parameters.size()),
            realtime_detail::direct_facts(function, remaining(), &source));
    }
    // Actor handlers/init fields are distinct runtime-generated ABI roots,
    // not ordinary fnc declarations. Do not leave their direct calls unscanned.
    for (std::size_t index = 0; index < source.actors.size(); ++index) {
        const auto& actor = source.actors[index];
        add("$atto.actor.handler." + std::to_string(index),
            realtime_detail::direct_facts(actor.on_message, remaining(), &source));
        realtime_detail::DirectFacts initializers;
        initializers.call_limit = remaining();
        for (const auto& field : actor.fields) {
            realtime_detail::scan_binding(field, initializers);
            if (initializers.call_budget_exceeded) break;
            if (field.new_class) {
                realtime_detail::scan_constructor_fields(
                    field, source, initializers);
                if (initializers.call_budget_exceeded) break;
            }
        }
        add("$atto.actor.fields." + std::to_string(index),
            std::move(initializers));
    }
    // Conservative treatment of nominal field defaults, including defaults
    // materialized by a native record constructor outside an explicit local.
    for (std::size_t index = 0; index < source.classes.size(); ++index) {
        realtime_detail::DirectFacts defaults;
        defaults.call_limit = remaining();
        for (const auto& field : source.classes[index].fields) {
            realtime_detail::scan_expression(
                field.initializer_expression.get(), defaults);
            if (defaults.call_budget_exceeded) break;
        }
        add("$atto.class.fields." + std::to_string(index),
            std::move(defaults));
    }
    (void)NativeGuestCallGraphGate::require_bounded(graph);
}

} // namespace oreslang
