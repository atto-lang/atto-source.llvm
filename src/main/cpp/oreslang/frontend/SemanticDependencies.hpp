#pragma once

#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang {

struct SemanticScanBudget {
    std::size_t max_ast_nodes = 100000;
    std::size_t max_ast_depth = 512;
    std::size_t max_dependency_edges = 30000;
};

struct SemanticDependencyFacts {
    std::set<CallableId> direct_calls;
    std::set<std::string> class_references;
    std::set<std::string> actor_spawns;
    std::size_t ast_nodes = 0;
};

namespace semantic_dependency_detail {

inline void consume_node(
        SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget,
        std::size_t depth) {
    if (depth > budget.max_ast_depth)
        throw std::runtime_error(
            "semantic dependency AST depth budget exceeded");
    if (++facts.ast_nodes > budget.max_ast_nodes)
        throw std::runtime_error(
            "semantic dependency AST node budget exceeded");
}

inline std::size_t dependency_count(const SemanticDependencyFacts& facts) {
    return facts.direct_calls.size() +
           facts.class_references.size() +
           facts.actor_spawns.size();
}

inline void check_edge_budget(
        const SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget) {
    if (dependency_count(facts) > budget.max_dependency_edges)
        throw std::runtime_error(
            "semantic dependency edge budget exceeded");
}

inline void scan_expression(
        const Expression* root,
        SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget,
        std::size_t base_depth = 1) {
    if (!root) return;

    std::vector<std::pair<const Expression*, std::size_t>> pending;
    pending.emplace_back(root, base_depth);
    while (!pending.empty()) {
        const auto [expr, depth] = pending.back();
        pending.pop_back();
        if (!expr) continue;

        consume_node(facts, budget, depth);
        if (expr->kind == Expression::Kind::Call) {
            facts.direct_calls.insert(
                callable_id(expr->name, expr->arguments.size()));
            check_edge_budget(facts, budget);
        }

        const std::size_t child_depth = depth + 1;
        if (expr->left)
            pending.emplace_back(expr->left.get(), child_depth);
        if (expr->right)
            pending.emplace_back(expr->right.get(), child_depth);
        if (expr->condition)
            pending.emplace_back(expr->condition.get(), child_depth);
        if (expr->condition_lhs)
            pending.emplace_back(expr->condition_lhs.get(), child_depth);
        if (expr->condition_rhs)
            pending.emplace_back(expr->condition_rhs.get(), child_depth);
        if (expr->when_true)
            pending.emplace_back(expr->when_true.get(), child_depth);
        if (expr->when_false)
            pending.emplace_back(expr->when_false.get(), child_depth);
        for (const auto& argument : expr->arguments)
            if (argument)
                pending.emplace_back(argument.get(), child_depth);
    }
}

inline void note_class_reference(
        const std::string& class_name,
        SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget) {
    if (class_name.empty()) return;
    facts.class_references.insert(class_name);
    check_edge_budget(facts, budget);
}

inline void scan_binding(
        const LocalBinding& binding,
        SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget,
        std::size_t depth) {
    consume_node(facts, budget, depth);
    note_class_reference(binding.class_name, facts, budget);
    scan_expression(binding.initializer.get(), facts, budget, depth + 1);
    for (const auto& argument : binding.constructor_arguments)
        scan_expression(argument.get(), facts, budget, depth + 1);
}

inline void scan_statement(
        const Statement& root,
        SemanticDependencyFacts& facts,
        const SemanticScanBudget& budget,
        std::size_t base_depth = 1) {
    std::vector<std::pair<const Statement*, std::size_t>> pending;
    pending.emplace_back(&root, base_depth);

    while (!pending.empty()) {
        const auto [statement, depth] = pending.back();
        pending.pop_back();
        consume_node(facts, budget, depth);

        scan_expression(statement->expression.get(), facts, budget, depth + 1);
        scan_expression(statement->condition.get(), facts, budget, depth + 1);
        scan_expression(statement->condition_lhs.get(), facts, budget, depth + 1);
        scan_expression(statement->condition_rhs.get(), facts, budget, depth + 1);
        scan_expression(statement->array_commit_length.get(), facts, budget, depth + 1);
        for (const auto& value : statement->array_commit_values)
            scan_expression(value.get(), facts, budget, depth + 1);
        if (statement->branch_local)
            scan_binding(*statement->branch_local, facts, budget, depth + 1);

        const std::size_t child_depth = depth + 1;
        for (const auto& nested : statement->then_statements)
            pending.emplace_back(&nested, child_depth);
        for (const auto& nested : statement->else_statements)
            pending.emplace_back(&nested, child_depth);
    }
}

} // namespace semantic_dependency_detail

inline SemanticDependencyFacts scan_semantic_dependencies(
        const Function& function,
        const SemanticScanBudget& budget = {}) {
    SemanticDependencyFacts facts;

    for (const auto& parameter : function.parameters)
        semantic_dependency_detail::note_class_reference(
            parameter.class_name, facts, budget);

    for (const auto& local : function.locals)
        semantic_dependency_detail::scan_binding(local, facts, budget, 1);

    for (const auto& statement : function.statements)
        semantic_dependency_detail::scan_statement(
            statement, facts, budget, 1);

    for (const auto& guard : function.early_returns) {
        semantic_dependency_detail::consume_node(facts, budget, 1);
        semantic_dependency_detail::scan_expression(
            guard.condition.get(), facts, budget, 2);
        semantic_dependency_detail::scan_expression(
            guard.lhs.get(), facts, budget, 2);
        semantic_dependency_detail::scan_expression(
            guard.rhs.get(), facts, budget, 2);
        semantic_dependency_detail::scan_expression(
            guard.result.get(), facts, budget, 2);
    }

    for (const auto& spawn : function.actor_spawns) {
        semantic_dependency_detail::consume_node(facts, budget, 1);
        if (spawn.actor_type.empty())
            throw std::runtime_error(
                "semantic dependency actor spawn lacks actor type");
        facts.actor_spawns.insert(spawn.actor_type);
        semantic_dependency_detail::check_edge_budget(facts, budget);
    }

    for (const auto& send : function.actor_sends) {
        semantic_dependency_detail::consume_node(facts, budget, 1);
        semantic_dependency_detail::scan_expression(
            send.payload.get(), facts, budget, 2);
    }

    for (const auto& write : function.actor_field_writes) {
        semantic_dependency_detail::consume_node(facts, budget, 1);
        semantic_dependency_detail::scan_expression(
            write.expression.get(), facts, budget, 2);
    }

    semantic_dependency_detail::scan_expression(
        function.caught_throw_value.get(), facts, budget, 1);
    for (const auto& statement : function.caught_throw_defers)
        semantic_dependency_detail::scan_statement(
            statement, facts, budget, 1);
    semantic_dependency_detail::scan_expression(
        function.result.get(), facts, budget, 1);

    return facts;
}

inline SemanticDependencyFacts scan_semantic_dependencies(
        const ClassDecl& klass,
        const SemanticScanBudget& budget = {}) {
    SemanticDependencyFacts facts;
    for (const auto& field : klass.fields) {
        semantic_dependency_detail::consume_node(facts, budget, 1);
        semantic_dependency_detail::scan_expression(
            field.initializer_expression.get(), facts, budget, 2);
    }
    return facts;
}

struct SemanticActorDependencyFacts {
    SemanticDependencyFacts initializers;
    SemanticDependencyFacts handler;
};

inline SemanticActorDependencyFacts scan_actor_semantic_dependencies(
        const ActorDecl& actor,
        const SemanticScanBudget& budget = {}) {
    SemanticActorDependencyFacts facts;
    for (const auto& field : actor.fields)
        semantic_dependency_detail::scan_binding(
            field, facts.initializers, budget, 1);
    facts.handler = scan_semantic_dependencies(
        actor.on_message, budget);
    if (facts.initializers.ast_nodes >
            budget.max_ast_nodes - facts.handler.ast_nodes)
        throw std::runtime_error(
            "semantic dependency AST node budget exceeded");
    if (semantic_dependency_detail::dependency_count(facts.initializers) >
            budget.max_dependency_edges -
            std::min(
                budget.max_dependency_edges,
                semantic_dependency_detail::dependency_count(facts.handler)))
        throw std::runtime_error(
            "semantic dependency edge budget exceeded");
    return facts;
}

inline SemanticDependencyFacts scan_semantic_dependencies(
        const ActorDecl& actor,
        const SemanticScanBudget& budget = {}) {
    const auto split = scan_actor_semantic_dependencies(actor, budget);
    SemanticDependencyFacts facts = split.initializers;
    facts.ast_nodes += split.handler.ast_nodes;
    facts.direct_calls.insert(
        split.handler.direct_calls.begin(), split.handler.direct_calls.end());
    facts.class_references.insert(
        split.handler.class_references.begin(),
        split.handler.class_references.end());
    facts.actor_spawns.insert(
        split.handler.actor_spawns.begin(),
        split.handler.actor_spawns.end());
    semantic_dependency_detail::check_edge_budget(facts, budget);
    return facts;
}

} // namespace oreslang
