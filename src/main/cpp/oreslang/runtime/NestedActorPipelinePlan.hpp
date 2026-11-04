#pragma once

// Compiler-facing, source-free metadata planner for nested Actor.pipe segments.
// This does NOT parse .atto, start actors, reserve mailboxes, implement
// CONTROL/DATA routing, or certify executable LLVM pipeline parity.
#include "oreslang/runtime/ActorPipeContract.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

enum class PipeRestartStrategy { one_for_one, one_for_all, rest_for_one };

struct NestedPipePart {
    bool segment = false;
    PipeStage stage{};
    std::vector<NestedPipePart> children;
    std::optional<std::size_t> capacity;
    std::optional<PipeRestartStrategy> supervision;

    NestedPipePart(const NestedPipePart&) = delete;
    NestedPipePart& operator=(const NestedPipePart&) = delete;
    NestedPipePart(NestedPipePart&&) = default;
    NestedPipePart& operator=(NestedPipePart&&) = default;

    static NestedPipePart actor(PipeStage value) {
        NestedPipePart result;
        result.stage = std::move(value);
        return result;
    }

    static NestedPipePart group(
            std::vector<NestedPipePart> items,
            std::optional<std::size_t> edge_capacity = std::nullopt,
            std::optional<PipeRestartStrategy> policy = std::nullopt) {
        NestedPipePart result;
        result.segment = true;
        result.children = std::move(items);
        result.capacity = edge_capacity;
        result.supervision = policy;
        return result;
    }

private:
    NestedPipePart() = default;
};

struct NestedPipeLimits {
    std::size_t default_capacity = 32;
    std::size_t max_depth = 64;
    std::size_t max_stages = 1024;
    std::size_t max_edges = 1023;
    std::size_t max_scopes = 256;
    std::size_t max_edge_capacity = 65536;
    std::size_t max_total_slots = 1000000;
};

struct PlannedPipeActor {
    std::string path;
    std::string owner_scope; // "root" is virtual unless configured explicitly
    PipeStage stage;
};

struct PlannedPipeEdge {
    std::string source_path;
    std::string target_path;
    std::string owner_scope;
    std::string payload;
    std::size_t capacity;
};

struct PlannedPipeChild {
    std::string path;
    bool is_supervisor;
    std::size_t registration_order;
};

struct PlannedPipeScope {
    std::string path;
    std::string parent_path;
    bool physical_supervisor; // false means inherit enclosing supervisor
    std::optional<PipeRestartStrategy> policy;
    std::vector<PlannedPipeChild> children;
};

struct NestedPipePlan {
    std::vector<PlannedPipeActor> actors;
    std::vector<PlannedPipeEdge> edges;
    std::vector<PlannedPipeScope> scopes;
    std::size_t total_slots = 0;
};

// The limits themselves are bounded. A caller cannot opt out of safety
// admission by substituting max-size configuration values.
inline void check_nested_pipe_limits(const NestedPipeLimits& limits) {
    if (!limits.default_capacity || !limits.max_depth ||
            !limits.max_stages || !limits.max_edges || !limits.max_scopes ||
            !limits.max_edge_capacity || !limits.max_total_slots ||
            limits.default_capacity > limits.max_edge_capacity ||
            limits.default_capacity > 65536 || limits.max_depth > 64 ||
            limits.max_stages > 1024 || limits.max_edges > 1023 ||
            limits.max_scopes > 256 || limits.max_edge_capacity > 65536 ||
            limits.max_total_slots > 1000000)
        throw std::invalid_argument("invalid nested actor pipe resource limits");
}

inline NestedPipePlan check_nested_actor_pipeline(
        const NestedPipePart& root, const NestedPipeLimits& limits = {}) {
    check_nested_pipe_limits(limits);
    if (!root.segment)
        throw std::invalid_argument("nested actor pipeline root must be a segment");

    NestedPipePlan plan;
    struct Ends {
        std::size_t first;
        std::size_t last;
    };

    auto add_child = [&](std::size_t scope, std::string path, bool supervisor) {
        auto& children = plan.scopes.at(scope).children;
        if (children.size() >= limits.max_stages + limits.max_scopes)
            throw std::invalid_argument("nested actor pipe child registration budget");
        children.push_back({std::move(path), supervisor, children.size()});
    };

    // Root is a bookkeeping scope even when no supervisor is minted.
    // A configured group creates an actual child supervisor under the
    // authorized caller; a plain group has no runtime supervisor identity.
    auto walk = [&](auto&& self, const NestedPipePart& part,
                    std::string path, std::size_t scope,
                    std::size_t inherited_capacity,
                    std::size_t depth) -> Ends {
        if (!part.segment) {
            if (!part.children.empty() || part.capacity || part.supervision)
                throw std::invalid_argument("malformed actor stage descriptor");
            if (plan.actors.size() >= limits.max_stages)
                throw std::invalid_argument("nested actor pipe stage budget exceeded");
            const auto index = plan.actors.size();
            plan.actors.push_back({path, plan.scopes.at(scope).path, part.stage});
            add_child(scope, path, false);
            return {index, index};
        }

        if (depth > limits.max_depth)
            throw std::invalid_argument("nested actor pipe depth budget exceeded");
        if (part.children.empty())
            throw std::invalid_argument("empty nested actor pipeline");
        if (part.capacity && (*part.capacity == 0 || *part.capacity > limits.max_edge_capacity))
            throw std::invalid_argument("nested actor pipe capacity out of range");
        if (part.supervision) {
            switch (*part.supervision) {
                case PipeRestartStrategy::one_for_one:
                case PipeRestartStrategy::one_for_all:
                case PipeRestartStrategy::rest_for_one:
                    break;
                default:
                    throw std::invalid_argument("invalid nested actor pipe supervisor policy");
            }
        }
        const auto current_capacity = part.capacity.value_or(inherited_capacity);
        const bool scoped = path == "root" || part.supervision.has_value();
        std::size_t owner = scope;
        if (scoped) {
            if (plan.scopes.size() >= limits.max_scopes)
                throw std::invalid_argument("nested actor pipe supervisor scope budget exceeded");
            const auto parent_path = path == "root" ? "" : plan.scopes.at(scope).path;
            owner = plan.scopes.size();
            // A reference into plan.scopes can be invalidated by push_back.
            plan.scopes.push_back({path, parent_path, part.supervision.has_value(),
                                  part.supervision, {}});
            if (path != "root") add_child(scope, path, true);
        }

        std::optional<Ends> previous;
        std::optional<Ends> first;
        for (std::size_t i = 0; i < part.children.size(); ++i) {
            Ends current = self(self, part.children[i],
                                path + "/" + std::to_string(i), owner,
                                current_capacity, depth + 1);
            if (previous) {
                if (plan.edges.size() >= limits.max_edges)
                    throw std::invalid_argument("nested actor pipe edge budget exceeded");
                if (plan.total_slots > limits.max_total_slots ||
                        current_capacity > limits.max_total_slots - plan.total_slots)
                    throw std::invalid_argument("nested actor pipe aggregate slot budget exceeded");
                plan.total_slots += current_capacity;
                plan.edges.push_back({
                    plan.actors[previous->last].path,
                    plan.actors[current.first].path,
                    plan.scopes.at(owner).path,
                    plan.actors[previous->last].stage.outbound,
                    current_capacity
                });
            }
            if (!first) first = current;
            previous = current;
        }
        return {first->first, previous->last};
    };

    (void)walk(walk, root, "root", 0, limits.default_capacity, 1);
    if (plan.actors.size() < 2)
        throw std::invalid_argument("started actor pipeline requires at least two stages");
    std::vector<PipeStage> flattened;
    flattened.reserve(plan.actors.size());
    for (const auto& actor : plan.actors) flattened.push_back(actor.stage);
    // Reuse the existing compiler-visible nominal role/port checker.
    const auto connections = check_actor_pipeline(flattened);
    if (connections.size() != plan.edges.size())
        throw std::logic_error("nested pipe graph edge mismatch");
    for (auto& edge : plan.edges) {
        const auto it = std::find_if(plan.actors.begin(), plan.actors.end(),
            [&](const PlannedPipeActor& actor) { return actor.path == edge.source_path; });
        if (it == plan.actors.end()) throw std::logic_error("nested pipe source missing");
        const auto index = static_cast<std::size_t>(it - plan.actors.begin());
        if (index >= connections.size() || edge.target_path != plan.actors[index + 1].path ||
                edge.payload != connections[index].payload)
            throw std::invalid_argument("nested pipe stage linkage mismatch");
    }
    std::sort(plan.edges.begin(), plan.edges.end(),
        [&](const PlannedPipeEdge& a, const PlannedPipeEdge& b) {
            auto source_index = [&](const std::string& path) {
                for (std::size_t i = 0; i < plan.actors.size(); ++i)
                    if (plan.actors[i].path == path) return i;
                return plan.actors.size();
            };
            return source_index(a.source_path) < source_index(b.source_path);
        });
    return plan;
}

// Direct-child only. No runtime restart, failure notice or CONTROL admission.
inline std::vector<std::string> nested_pipe_restart_targets(
        const NestedPipePlan& plan, const std::string& scope_path,
        const std::string& failed_direct_child) {
    const auto it = std::find_if(plan.scopes.begin(), plan.scopes.end(),
        [&](const PlannedPipeScope& scope) { return scope.path == scope_path; });
    if (it == plan.scopes.end() || !it->physical_supervisor || !it->policy)
        throw std::invalid_argument("no explicit supervisor policy at nested pipe scope");
    const auto failed = std::find_if(it->children.begin(), it->children.end(),
        [&](const PlannedPipeChild& child) { return child.path == failed_direct_child; });
    if (failed == it->children.end())
        throw std::invalid_argument("failed child is not a direct supervisor child");
    std::size_t begin = 0, end = it->children.size();
    switch (*it->policy) {
        case PipeRestartStrategy::one_for_one:
            begin = static_cast<std::size_t>(failed - it->children.begin());
            end = begin + 1;
            break;
        case PipeRestartStrategy::one_for_all:
            break;
        case PipeRestartStrategy::rest_for_one:
            begin = static_cast<std::size_t>(failed - it->children.begin());
            break;
        default:
            throw std::invalid_argument("invalid nested pipe restart strategy");
    }
    std::vector<std::string> selected;
    for (std::size_t i = begin; i < end; ++i)
        selected.push_back(it->children[i].path);
    return selected;
}

} // namespace oreslang::runtime
