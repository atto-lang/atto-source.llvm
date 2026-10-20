#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang {

struct ReachabilityResult {
    std::vector<std::string> roots;
    std::vector<std::string> retained_symbols;
    std::vector<std::string> removed_symbols;
};

namespace reachability_detail {

inline std::string qualify(const Program& program, const std::string& name) {
    return (program.module.empty() ? std::string("__root__") : program.module) + "." + name;
}

inline void scan_expression(
        const Expression* expr,
        const std::map<std::string, const Function*>& functions,
        std::set<std::string>& function_dependencies) {
    if (!expr) return;
    if (expr->kind == Expression::Kind::Call) {
        if (functions.find(expr->name) == functions.end()) {
            throw std::runtime_error("native reachability unresolved function call: " + expr->name);
        }
        function_dependencies.insert(expr->name);
    }
    scan_expression(expr->left.get(), functions, function_dependencies);
    scan_expression(expr->right.get(), functions, function_dependencies);
    for (const auto& arg : expr->arguments)
        scan_expression(arg.get(), functions, function_dependencies);
}

inline void scan_statement(
        const Statement& statement,
        const std::map<std::string, const Function*>& functions,
        std::set<std::string>& function_dependencies) {
    scan_expression(statement.expression.get(), functions, function_dependencies);
    scan_expression(statement.condition_lhs.get(), functions, function_dependencies);
    scan_expression(statement.condition_rhs.get(), functions, function_dependencies);
    scan_expression(statement.array_commit_length.get(), functions, function_dependencies);
    for (const auto& value : statement.array_commit_values)
        scan_expression(value.get(), functions, function_dependencies);
    if (statement.branch_local)
        scan_expression(statement.branch_local->initializer.get(), functions, function_dependencies);
    for (const auto& nested : statement.then_statements)
        scan_statement(nested, functions, function_dependencies);
    for (const auto& nested : statement.else_statements)
        scan_statement(nested, functions, function_dependencies);
}

inline void scan_function(
        const Function& fn,
        const std::map<std::string, const Function*>& functions,
        std::set<std::string>& function_dependencies,
        std::set<std::string>& actor_dependencies,
        std::set<std::string>& class_dependencies) {
    scan_expression(fn.result.get(), functions, function_dependencies);
    scan_expression(fn.caught_throw_value.get(), functions, function_dependencies);

    for (const auto& local : fn.locals) {
        scan_expression(local.initializer.get(), functions, function_dependencies);
        for (const auto& arg : local.constructor_arguments)
            scan_expression(arg.get(), functions, function_dependencies);
        if (!local.class_name.empty()) class_dependencies.insert(local.class_name);
    }
    for (const auto& statement : fn.statements)
        scan_statement(statement, functions, function_dependencies);
    for (const auto& early : fn.early_returns) {
        scan_expression(early.lhs.get(), functions, function_dependencies);
        scan_expression(early.rhs.get(), functions, function_dependencies);
        scan_expression(early.result.get(), functions, function_dependencies);
    }
    for (const auto& statement : fn.caught_throw_defers)
        scan_statement(statement, functions, function_dependencies);
    for (const auto& spawn : fn.actor_spawns)
        actor_dependencies.insert(spawn.actor_type);
    for (const auto& send : fn.actor_sends)
        scan_expression(send.payload.get(), functions, function_dependencies);
    for (const auto& write : fn.actor_field_writes)
        scan_expression(write.expression.get(), functions, function_dependencies);
    for (const auto& parameter : fn.parameters)
        if (!parameter.class_name.empty()) class_dependencies.insert(parameter.class_name);
}

inline std::vector<std::string> sorted(const std::set<std::string>& values) {
    return std::vector<std::string>(values.begin(), values.end());
}

} // namespace reachability_detail

/**
 * Restricted native frontend reachability analyzer.
 *
 * This intentionally covers only Program constructs already represented by the
 * native parser. Unknown direct calls fail closed. Backends must run this after
 * parsing/type admission and before target-specific lowering/linker DCE.
 */
inline ReachabilityResult analyze_reachability(
        const Program& program,
        const std::vector<std::string>& explicit_roots,
        bool preserve_exported_api) {
    std::map<std::string, const Function*> functions;
    std::map<std::string, const ActorDecl*> actors;
    std::set<std::string> classes;

    for (const auto& fn : program.functions) {
        if (!functions.emplace(fn.name, &fn).second)
            throw std::runtime_error("native reachability rejects overloaded function name: " + fn.name);
    }
    for (const auto& actor : program.actors) {
        if (!actors.emplace(actor.name, &actor).second)
            throw std::runtime_error("native reachability rejects duplicate actor name: " + actor.name);
    }
    for (const auto& klass : program.classes) {
        if (!classes.insert(klass.name).second)
            throw std::runtime_error("native reachability rejects duplicate class name: " + klass.name);
    }

    std::deque<std::pair<char, std::string>> work;
    std::set<std::string> retained_functions;
    std::set<std::string> retained_actors;
    std::set<std::string> retained_classes;
    std::set<std::string> root_symbols;

    const auto mark_function = [&](const std::string& name) {
        if (functions.find(name) == functions.end())
            throw std::runtime_error("native reachability root/function not found: " + name);
        if (retained_functions.insert(name).second) work.emplace_back('f', name);
    };
    const auto mark_actor = [&](const std::string& name) {
        if (actors.find(name) == actors.end())
            throw std::runtime_error("native reachability actor not found: " + name);
        if (retained_actors.insert(name).second) work.emplace_back('a', name);
    };
    const auto mark_class = [&](const std::string& name) {
        if (classes.find(name) == classes.end())
            throw std::runtime_error("native reachability class not found: " + name);
        retained_classes.insert(name);
    };

    for (const auto& root : explicit_roots) {
        mark_function(root);
        root_symbols.insert(reachability_detail::qualify(program, root));
    }
    if (preserve_exported_api) {
        for (const auto& [name, fn] : functions) {
            if (!fn->exported) continue;
            mark_function(name);
            root_symbols.insert(reachability_detail::qualify(program, name));
        }
    }

    while (!work.empty()) {
        const auto [kind, name] = work.front();
        work.pop_front();

        std::set<std::string> function_dependencies;
        std::set<std::string> actor_dependencies;
        std::set<std::string> class_dependencies;

        if (kind == 'f') {
            reachability_detail::scan_function(
                    *functions.at(name),
                    functions,
                    function_dependencies,
                    actor_dependencies,
                    class_dependencies);
        } else {
            const auto& actor = *actors.at(name);
            for (const auto& field : actor.fields) {
                reachability_detail::scan_expression(
                        field.initializer.get(), functions, function_dependencies);
                for (const auto& arg : field.constructor_arguments)
                    reachability_detail::scan_expression(
                            arg.get(), functions, function_dependencies);
                if (!field.class_name.empty()) class_dependencies.insert(field.class_name);
            }
            reachability_detail::scan_function(
                    actor.on_message,
                    functions,
                    function_dependencies,
                    actor_dependencies,
                    class_dependencies);
        }

        for (const auto& dep : function_dependencies) mark_function(dep);
        for (const auto& dep : actor_dependencies) mark_actor(dep);
        for (const auto& dep : class_dependencies) mark_class(dep);
    }

    std::set<std::string> retained;
    std::set<std::string> removed;

    for (const auto& [name, fn] : functions) {
        (void)fn;
        (retained_functions.count(name) ? retained : removed)
                .insert(reachability_detail::qualify(program, name));
    }
    for (const auto& [name, actor] : actors) {
        (void)actor;
        (retained_actors.count(name) ? retained : removed)
                .insert(reachability_detail::qualify(program, name));
    }
    for (const auto& name : classes) {
        (retained_classes.count(name) ? retained : removed)
                .insert(reachability_detail::qualify(program, name));
    }

    return ReachabilityResult{
        reachability_detail::sorted(root_symbols),
        reachability_detail::sorted(retained),
        reachability_detail::sorted(removed)};
}

} // namespace oreslang
