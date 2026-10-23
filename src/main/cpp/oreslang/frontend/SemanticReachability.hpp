#pragma once

#include "oreslang/frontend/SemanticDependencies.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang {

enum class SemanticDeclarationKind {
    Function,
    Class,
    Actor,
    Continuation
};

struct SemanticDeclarationId {
    SemanticDeclarationKind kind = SemanticDeclarationKind::Function;
    std::string name;
    std::size_t arity = 0;

    friend bool operator==(const SemanticDeclarationId& lhs,
                           const SemanticDeclarationId& rhs) {
        return std::tie(lhs.kind, lhs.name, lhs.arity) ==
               std::tie(rhs.kind, rhs.name, rhs.arity);
    }
    friend bool operator<(const SemanticDeclarationId& lhs,
                          const SemanticDeclarationId& rhs) {
        return std::tie(lhs.kind, lhs.name, lhs.arity) <
               std::tie(rhs.kind, rhs.name, rhs.arity);
    }
};

inline SemanticDeclarationId function_declaration_id(const CallableId& callable) {
    return {
        SemanticDeclarationKind::Function,
        callable.name,
        callable.arity};
}

inline SemanticDeclarationId class_declaration_id(std::string name) {
    return {SemanticDeclarationKind::Class, std::move(name), 0};
}

inline SemanticDeclarationId actor_declaration_id(std::string name) {
    return {SemanticDeclarationKind::Actor, std::move(name), 0};
}

inline SemanticDeclarationId continuation_declaration_id(std::string name) {
    return {SemanticDeclarationKind::Continuation, std::move(name), 0};
}

inline std::string semantic_declaration_key(const SemanticDeclarationId& id) {
    switch (id.kind) {
        case SemanticDeclarationKind::Function:
            return "function:" + analysis_callable_key(
                CallableId{id.name, id.arity});
        case SemanticDeclarationKind::Class:
            return "class:" + id.name;
        case SemanticDeclarationKind::Actor:
            return "actor:" + id.name;
        case SemanticDeclarationKind::Continuation:
            return "continuation:" + id.name;
    }
    throw std::runtime_error("unknown semantic declaration kind");
}

enum class SemanticBuildKind {
    Executable,
    Library
};

enum class SemanticReachabilityReason {
    RootEntry,
    RootExplicit,
    RootPublicApi,
    RootTargetAbi,
    ContinuationAbi,
    DirectCall,
    ActorSpawn,
    ActorHandler,
    ClassReference,
    ClassInitializer
};

inline const char* semantic_reachability_reason_name(
        SemanticReachabilityReason reason) {
    switch (reason) {
        case SemanticReachabilityReason::RootEntry: return "ROOT_ENTRY";
        case SemanticReachabilityReason::RootExplicit: return "ROOT_EXPLICIT";
        case SemanticReachabilityReason::RootPublicApi: return "ROOT_PUBLIC_API";
        case SemanticReachabilityReason::RootTargetAbi: return "ROOT_TARGET_ABI";
        case SemanticReachabilityReason::ContinuationAbi: return "CONTINUATION_ABI";
        case SemanticReachabilityReason::DirectCall: return "DIRECT_CALL";
        case SemanticReachabilityReason::ActorSpawn: return "ACTOR_SPAWN";
        case SemanticReachabilityReason::ActorHandler: return "ACTOR_HANDLER";
        case SemanticReachabilityReason::ClassReference: return "CLASS_REFERENCE";
        case SemanticReachabilityReason::ClassInitializer: return "CLASS_INITIALIZER";
    }
    throw std::runtime_error("unknown semantic reachability reason");
}

struct SemanticRootReason {
    SemanticDeclarationId declaration;
    SemanticReachabilityReason reason;
};

struct SemanticReachabilityEdge {
    SemanticDeclarationId from;
    SemanticDeclarationId to;
    SemanticReachabilityReason reason;

    friend bool operator<(const SemanticReachabilityEdge& lhs,
                          const SemanticReachabilityEdge& rhs) {
        return std::tie(lhs.from, lhs.to, lhs.reason) <
               std::tie(rhs.from, rhs.to, rhs.reason);
    }
};

struct SemanticRootPolicy {
    SemanticBuildKind build_kind = SemanticBuildKind::Executable;
    std::vector<CallableId> explicit_function_roots;
    std::vector<std::string> target_actor_roots;
    std::vector<std::string> target_class_roots;
    std::vector<std::string> target_continuation_roots;
    bool root_default_main = true;
};

struct SemanticGraphBudget {
    std::size_t max_declarations = 100000;
    std::size_t max_edges = 30000;
    SemanticScanBudget scan{};
};

struct SemanticReachabilityResult {
    std::vector<SemanticDeclarationId> roots;
    std::vector<SemanticDeclarationId> retained;
    std::vector<SemanticDeclarationId> removed;
    std::vector<SemanticRootReason> root_reasons;
    std::vector<SemanticReachabilityEdge> edges;
};

struct SemanticReachabilityExplanation {
    SemanticDeclarationId declaration;
    bool root = false;
    std::vector<SemanticReachabilityReason> root_reasons;
    std::vector<SemanticReachabilityEdge> incoming_edges;
};

namespace semantic_reachability_detail {

struct Node {
    const Function* function = nullptr;
    const ClassDecl* klass = nullptr;
    const ActorDecl* actor = nullptr;
};

inline void require_known(
        const std::map<SemanticDeclarationId, Node>& nodes,
        const SemanticDeclarationId& id,
        const char* what) {
    if (!nodes.count(id))
        throw std::runtime_error(
            std::string("semantic reachability unresolved ") + what +
            ": " + semantic_declaration_key(id));
}

inline void add_dependency_edges(
        const SemanticDeclarationId& from,
        const SemanticDependencyFacts& facts,
        const std::map<SemanticDeclarationId, Node>& nodes,
        std::set<SemanticReachabilityEdge>& edges,
        const SemanticGraphBudget& budget,
        SemanticReachabilityReason call_reason) {
    for (const auto& call : facts.direct_calls) {
        const auto to = function_declaration_id(call);
        require_known(nodes, to, "direct call");
        edges.insert({from, to, call_reason});
    }
    for (const auto& klass : facts.class_references) {
        const auto to = class_declaration_id(klass);
        require_known(nodes, to, "class reference");
        edges.insert({
            from,
            to,
            SemanticReachabilityReason::ClassReference});
    }
    for (const auto& actor : facts.actor_spawns) {
        const auto to = actor_declaration_id(actor);
        require_known(nodes, to, "actor spawn");
        edges.insert({
            from,
            to,
            SemanticReachabilityReason::ActorSpawn});
    }
    if (edges.size() > budget.max_edges)
        throw std::runtime_error(
            "semantic reachability graph edge budget exceeded");
}

inline void add_root(
        const SemanticDeclarationId& id,
        SemanticReachabilityReason reason,
        const std::map<SemanticDeclarationId, Node>& nodes,
        std::set<SemanticDeclarationId>& roots,
        std::set<std::tuple<SemanticDeclarationId, SemanticReachabilityReason>>&
            root_reasons) {
    require_known(nodes, id, "root");
    roots.insert(id);
    root_reasons.emplace(id, reason);
}

} // namespace semantic_reachability_detail

inline SemanticReachabilityResult analyze_semantic_reachability(
        const Program& program,
        const SemanticRootPolicy& policy = {},
        const SemanticGraphBudget& budget = {}) {
    using namespace semantic_reachability_detail;

    std::map<SemanticDeclarationId, Node> nodes;
    std::map<CallableId, const Function*> functions;

    auto add_node = [&](const SemanticDeclarationId& id, Node node) {
        if (nodes.size() >= budget.max_declarations)
            throw std::runtime_error(
                "semantic reachability declaration budget exceeded");
        if (!nodes.emplace(id, node).second)
            throw std::runtime_error(
                "duplicate semantic declaration identity: " +
                semantic_declaration_key(id));
    };

    for (const auto& function : program.functions) {
        const CallableId callable{
            function.name, function.parameters.size()};
        if (!functions.emplace(callable, &function).second)
            throw std::runtime_error(
                "duplicate semantic callable identity: " +
                analysis_callable_key(callable));
        add_node(
            function_declaration_id(callable),
            Node{&function, nullptr, nullptr});
    }
    for (const auto& klass : program.classes)
        add_node(
            class_declaration_id(klass.name),
            Node{nullptr, &klass, nullptr});
    for (const auto& actor : program.actors)
        add_node(
            actor_declaration_id(actor.name),
            Node{nullptr, nullptr, &actor});
    for (const auto& loop : program.cooperative_loops)
        add_node(continuation_declaration_id(loop.name), Node{});

    std::set<SemanticReachabilityEdge> edges;

    for (const auto& [id, node] : nodes) {
        if (node.function) {
            const auto facts = scan_semantic_dependencies(
                *node.function, budget.scan);
            add_dependency_edges(
                id, facts, nodes, edges, budget,
                SemanticReachabilityReason::DirectCall);
        } else if (node.klass) {
            const auto facts = scan_semantic_dependencies(
                *node.klass, budget.scan);
            add_dependency_edges(
                id, facts, nodes, edges, budget,
                SemanticReachabilityReason::ClassInitializer);
        } else if (node.actor) {
            const auto facts = scan_semantic_dependencies(
                *node.actor, budget.scan);
            add_dependency_edges(
                id, facts, nodes, edges, budget,
                SemanticReachabilityReason::ActorHandler);
        }
    }

    std::set<SemanticDeclarationId> roots;
    std::set<std::tuple<SemanticDeclarationId, SemanticReachabilityReason>>
        root_reason_set;

    for (const auto& callable : policy.explicit_function_roots)
        add_root(
            function_declaration_id(callable),
            SemanticReachabilityReason::RootExplicit,
            nodes, roots, root_reason_set);

    if (policy.explicit_function_roots.empty()) {
        if (policy.build_kind == SemanticBuildKind::Executable &&
                policy.root_default_main) {
            const auto main = function_declaration_id(
                CallableId{"main", 0});
            if (nodes.count(main))
                add_root(
                    main,
                    SemanticReachabilityReason::RootEntry,
                    nodes, roots, root_reason_set);
        }
        if (policy.build_kind == SemanticBuildKind::Library) {
            for (const auto& [callable, function] : functions) {
                if (!function->exported) continue;
                add_root(
                    function_declaration_id(callable),
                    SemanticReachabilityReason::RootPublicApi,
                    nodes, roots, root_reason_set);
            }
        }
    }

    for (const auto& actor : policy.target_actor_roots)
        add_root(
            actor_declaration_id(actor),
            SemanticReachabilityReason::RootTargetAbi,
            nodes, roots, root_reason_set);
    for (const auto& klass : policy.target_class_roots)
        add_root(
            class_declaration_id(klass),
            SemanticReachabilityReason::RootTargetAbi,
            nodes, roots, root_reason_set);
    for (const auto& continuation : policy.target_continuation_roots)
        add_root(
            continuation_declaration_id(continuation),
            SemanticReachabilityReason::RootTargetAbi,
            nodes, roots, root_reason_set);

    // Both current native continuation forms are host-driven ABI surfaces.
    for (const auto& function : program.functions) {
        if (!function.scalar_while) continue;
        add_root(
            function_declaration_id(
                CallableId{function.name, function.parameters.size()}),
            SemanticReachabilityReason::ContinuationAbi,
            nodes, roots, root_reason_set);
    }
    for (const auto& loop : program.cooperative_loops)
        add_root(
            continuation_declaration_id(loop.name),
            SemanticReachabilityReason::ContinuationAbi,
            nodes, roots, root_reason_set);

    if (!nodes.empty() && roots.empty())
        throw std::runtime_error(
            "semantic reachability has declarations but no roots");

    std::map<SemanticDeclarationId, std::vector<SemanticReachabilityEdge>>
        outgoing;
    for (const auto& [id, node] : nodes) {
        (void)node;
        outgoing.emplace(id, std::vector<SemanticReachabilityEdge>{});
    }
    for (const auto& edge : edges)
        outgoing.at(edge.from).push_back(edge);

    std::set<SemanticDeclarationId> retained;
    std::vector<SemanticDeclarationId> pending(
        roots.begin(), roots.end());
    retained.insert(roots.begin(), roots.end());

    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto& from = pending[cursor];
        for (const auto& edge : outgoing.at(from)) {
            if (retained.insert(edge.to).second)
                pending.push_back(edge.to);
        }
    }

    std::vector<SemanticDeclarationId> removed;
    for (const auto& [id, node] : nodes) {
        (void)node;
        if (!retained.count(id))
            removed.push_back(id);
    }

    SemanticReachabilityResult result;
    result.roots.assign(roots.begin(), roots.end());
    result.retained.assign(retained.begin(), retained.end());
    result.removed = std::move(removed);
    result.edges.assign(edges.begin(), edges.end());
    for (const auto& item : root_reason_set)
        result.root_reasons.push_back(
            {std::get<0>(item), std::get<1>(item)});

    // Analyzer-produced provenance must explain every retained declaration.
    for (const auto& id : result.retained) {
        if (roots.count(id)) continue;
        const bool explained = std::any_of(
            result.edges.begin(), result.edges.end(),
            [&](const SemanticReachabilityEdge& edge) {
                return retained.count(edge.from) && edge.to == id;
            });
        if (!explained)
            throw std::runtime_error(
                "retained semantic declaration lacks provenance: " +
                semantic_declaration_key(id));
    }

    return result;
}

inline SemanticReachabilityExplanation explain_semantic_reachability(
        const SemanticReachabilityResult& result,
        const SemanticDeclarationId& declaration) {
    if (!std::binary_search(
            result.retained.begin(), result.retained.end(), declaration))
        throw std::runtime_error(
            "cannot explain non-retained semantic declaration: " +
            semantic_declaration_key(declaration));

    SemanticReachabilityExplanation explanation;
    explanation.declaration = declaration;
    explanation.root = std::binary_search(
        result.roots.begin(), result.roots.end(), declaration);
    for (const auto& root : result.root_reasons)
        if (root.declaration == declaration)
            explanation.root_reasons.push_back(root.reason);
    for (const auto& edge : result.edges)
        if (edge.to == declaration &&
                std::binary_search(
                    result.retained.begin(),
                    result.retained.end(),
                    edge.from))
            explanation.incoming_edges.push_back(edge);
    return explanation;
}

} // namespace oreslang
