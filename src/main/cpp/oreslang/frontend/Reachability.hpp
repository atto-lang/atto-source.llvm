#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
        std::set<std::string>& function_dependencies,
        std::set<std::string>& class_dependencies) {
    scan_expression(statement.expression.get(), functions, function_dependencies);
    scan_expression(statement.condition_lhs.get(), functions, function_dependencies);
    scan_expression(statement.condition_rhs.get(), functions, function_dependencies);
    scan_expression(statement.array_commit_length.get(), functions, function_dependencies);
    for (const auto& value : statement.array_commit_values)
        scan_expression(value.get(), functions, function_dependencies);
    if (statement.branch_local) {
        scan_expression(statement.branch_local->initializer.get(), functions, function_dependencies);
        for (const auto& arg : statement.branch_local->constructor_arguments)
            scan_expression(arg.get(), functions, function_dependencies);
        if (!statement.branch_local->class_name.empty())
            class_dependencies.insert(statement.branch_local->class_name);
    }
    for (const auto& nested : statement.then_statements)
        scan_statement(nested, functions, function_dependencies, class_dependencies);
    for (const auto& nested : statement.else_statements)
        scan_statement(nested, functions, function_dependencies, class_dependencies);
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
        scan_statement(statement, functions, function_dependencies, class_dependencies);
    for (const auto& early : fn.early_returns) {
        scan_expression(early.lhs.get(), functions, function_dependencies);
        scan_expression(early.rhs.get(), functions, function_dependencies);
        scan_expression(early.result.get(), functions, function_dependencies);
    }
    for (const auto& statement : fn.caught_throw_defers)
        scan_statement(statement, functions, function_dependencies, class_dependencies);
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
    std::map<std::string, const ClassDecl*> classes;

    for (const auto& fn : program.functions) {
        if (!functions.emplace(fn.name, &fn).second)
            throw std::runtime_error("native reachability rejects overloaded function name: " + fn.name);
    }
    for (const auto& actor : program.actors) {
        if (!actors.emplace(actor.name, &actor).second)
            throw std::runtime_error("native reachability rejects duplicate actor name: " + actor.name);
    }
    for (const auto& klass : program.classes) {
        if (!classes.emplace(klass.name, &klass).second)
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
        if (retained_classes.insert(name).second) work.emplace_back('c', name);
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
        } else if (kind == 'a') {
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
        } else if (kind == 'c') {
            const auto& klass = *classes.at(name);
            for (const auto& field : klass.fields)
                reachability_detail::scan_expression(
                        field.initializer_expression.get(), functions, function_dependencies);
        } else {
            throw std::runtime_error("native reachability internal work-kind corruption");
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
    for (const auto& [name, klass] : classes) {
        (void)klass;
        (retained_classes.count(name) ? retained : removed)
                .insert(reachability_detail::qualify(program, name));
    }

    return ReachabilityResult{
        reachability_detail::sorted(root_symbols),
        reachability_detail::sorted(retained),
        reachability_detail::sorted(removed)};
}


inline void append_reachability_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4) & 0x0f]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
}

inline void append_reachability_json_array(
        std::string& out, const std::vector<std::string>& values) {
    out.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_reachability_json_string(out, values[i]);
    }
    out.push_back(']');
}

inline bool reachability_strictly_sorted_unique(
        const std::vector<std::string>& values) {
    for (std::size_t i = 1; i < values.size(); ++i)
        if (!(values[i - 1] < values[i])) return false;
    return true;
}

inline void validate_reachability_result(const ReachabilityResult& result) {
    if (!reachability_strictly_sorted_unique(result.roots) ||
        !reachability_strictly_sorted_unique(result.retained_symbols) ||
        !reachability_strictly_sorted_unique(result.removed_symbols))
        throw std::runtime_error("native reachability manifest requires sorted unique symbol sets");

    for (const auto& root : result.roots) {
        if (!std::binary_search(
                result.retained_symbols.begin(), result.retained_symbols.end(), root))
            throw std::runtime_error("native reachability root is not retained: " + root);
    }

    std::size_t retained = 0;
    std::size_t removed = 0;
    while (retained < result.retained_symbols.size() &&
           removed < result.removed_symbols.size()) {
        if (result.retained_symbols[retained] == result.removed_symbols[removed])
            throw std::runtime_error(
                "native reachability symbol appears in retained and removed sets: " +
                result.retained_symbols[retained]);
        if (result.retained_symbols[retained] < result.removed_symbols[removed])
            ++retained;
        else
            ++removed;
    }
}

inline std::string canonical_reachability_json(
        const ReachabilityResult& result, bool library_build) {
    validate_reachability_result(result);
    std::string out;
    out.reserve(256);
    out += "{\"format\":\"ores-reachability/1\",\"buildKind\":";
    append_reachability_json_string(out, library_build ? "library" : "executable");
    out += ",\"roots\":";
    append_reachability_json_array(out, result.roots);
    out += ",\"retainedSymbols\":";
    append_reachability_json_array(out, result.retained_symbols);
    out += ",\"removedSymbols\":";
    append_reachability_json_array(out, result.removed_symbols);
    out += ",\"compileTimeConstants\":{}}";
    return out;
}

} // namespace oreslang
