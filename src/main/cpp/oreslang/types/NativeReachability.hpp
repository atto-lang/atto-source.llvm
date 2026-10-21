#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oreslang {
namespace reachability_detail {
inline void collect_tree_shake_expression_calls(
        const oreslang::Expression& expression,
        std::unordered_set<std::string>& calls) {
    using Kind = oreslang::Expression::Kind;
    if (expression.kind == Kind::Call)
        calls.insert(oreslang::native_function_slot(
            expression.name, expression.arguments.size()));
    if (expression.left)
        collect_tree_shake_expression_calls(*expression.left, calls);
    if (expression.right)
        collect_tree_shake_expression_calls(*expression.right, calls);
    for (const auto& argument : expression.arguments)
        if (argument) collect_tree_shake_expression_calls(*argument, calls);
}

inline void collect_tree_shake_statement_calls(
        const oreslang::Statement& statement,
        std::unordered_set<std::string>& calls) {
    if (statement.expression)
        collect_tree_shake_expression_calls(*statement.expression, calls);
    if (statement.branch_local) {
        if (statement.branch_local->initializer)
            collect_tree_shake_expression_calls(
                *statement.branch_local->initializer, calls);
        for (const auto& argument : statement.branch_local->constructor_arguments)
            if (argument) collect_tree_shake_expression_calls(*argument, calls);
    }
    if (statement.array_commit_length)
        collect_tree_shake_expression_calls(*statement.array_commit_length, calls);
    for (const auto& value : statement.array_commit_values)
        if (value) collect_tree_shake_expression_calls(*value, calls);
    if (statement.condition_lhs)
        collect_tree_shake_expression_calls(*statement.condition_lhs, calls);
    if (statement.condition_rhs)
        collect_tree_shake_expression_calls(*statement.condition_rhs, calls);
    for (const auto& nested : statement.then_statements)
        collect_tree_shake_statement_calls(nested, calls);
    for (const auto& nested : statement.else_statements)
        collect_tree_shake_statement_calls(nested, calls);
}

inline std::unordered_set<std::string> tree_shake_function_calls(
        const oreslang::Function& function) {
    std::unordered_set<std::string> calls;
    for (const auto& local : function.locals) {
        if (local.initializer)
            collect_tree_shake_expression_calls(*local.initializer, calls);
        for (const auto& argument : local.constructor_arguments)
            if (argument) collect_tree_shake_expression_calls(*argument, calls);
    }
    for (const auto& statement : function.statements)
        collect_tree_shake_statement_calls(statement, calls);
    for (const auto& guarded : function.early_returns) {
        if (guarded.lhs) collect_tree_shake_expression_calls(*guarded.lhs, calls);
        if (guarded.rhs) collect_tree_shake_expression_calls(*guarded.rhs, calls);
        if (guarded.result) collect_tree_shake_expression_calls(*guarded.result, calls);
    }
    for (const auto& send : function.actor_sends)
        if (send.payload) collect_tree_shake_expression_calls(*send.payload, calls);
    for (const auto& write : function.actor_field_writes)
        if (write.expression)
            collect_tree_shake_expression_calls(*write.expression, calls);
    if (function.caught_throw_value)
        collect_tree_shake_expression_calls(*function.caught_throw_value, calls);
    for (const auto& statement : function.caught_throw_defers)
        collect_tree_shake_statement_calls(statement, calls);
    if (function.result)
        collect_tree_shake_expression_calls(*function.result, calls);
    return calls;
}

} // namespace reachability_detail

inline void tree_shake_functions(oreslang::Program& program, const std::vector<std::string>& requested_roots) {
    std::unordered_map<std::string, std::size_t> definitions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        const std::string key =
            oreslang::native_function_slot(function.name, function.parameters.size());
        if (!definitions.emplace(key, i).second)
            throw std::runtime_error(
                "tree shaking requires unique native function name/arity identities");
    }

    std::vector<std::string> pending;
    std::unordered_set<std::string> reachable;
    if (!requested_roots.empty()) {
        for (const auto& requested : requested_roots) {
            std::vector<std::string> matches;
            for (const auto& function : program.functions) {
                if (function.name == requested)
                    matches.push_back(oreslang::native_function_slot(
                        function.name, function.parameters.size()));
            }
            if (matches.empty())
                throw std::runtime_error("unknown tree-shake root: " + requested);
            if (matches.size() != 1)
                throw std::runtime_error("ambiguous tree-shake root: " + requested);
            if (reachable.insert(matches.front()).second) pending.push_back(matches.front());
        }
    } else {
        for (const auto& function : program.functions) {
            if (function.exported || function.name == "main") {
                const std::string key =
                    oreslang::native_function_slot(function.name, function.parameters.size());
                if (reachable.insert(key).second) pending.push_back(key);
            }
        }
    }
    // Every declared actor emits a native initializer and on_message symbol.
    // Their direct/transitive callees are AOT roots even when no ordinary
    // function calls them: the scheduler invokes these callbacks via the ABI.
    // Until actor declarations themselves are pruned, retain all such roots.
    for (const auto& actor : program.actors) {
        auto calls = reachability_detail::tree_shake_function_calls(actor.on_message);
        for (const auto& field : actor.fields)
            if (field.initializer)
                reachability_detail::collect_tree_shake_expression_calls(*field.initializer, calls);
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error("tree shaking found an unresolved actor callback call: " + call);
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }
    if (pending.empty() && program.actors.empty())
        throw std::runtime_error(
            "tree shaking requires an explicit root, main, or at least one pub function");

    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto definition = definitions.find(pending[cursor]);
        if (definition == definitions.end())
            throw std::runtime_error("tree shaking found an unresolved function root");
        const auto calls =
            reachability_detail::tree_shake_function_calls(program.functions[definition->second]);
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error(
                    "tree shaking found an unresolved direct call: " + call);
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const oreslang::Function& function) {
                return !reachable.count(oreslang::native_function_slot(
                    function.name, function.parameters.size()));
            }),
        program.functions.end());
}


} // namespace oreslang
