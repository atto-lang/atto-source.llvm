#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/CallableSelector.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang {

enum class ReachabilityReason {
    RootEntry,
    RootExplicit,
    RootPublicApi,
    RootTargetAbi,
    ContinuationAbi,
    DirectCall,
    ActorSpawn,
    ClassReference,
    ActorBody,
    ClassInitializer,
    ExportedOverloadAbi
};

struct ReachabilityRootReason {
    std::string symbol;
    ReachabilityReason reason;
};

struct ReachabilityEdge {
    std::string from;
    std::string to;
    ReachabilityReason reason;
};

inline const char* reachability_reason_name(ReachabilityReason reason) {
    switch (reason) {
        case ReachabilityReason::RootEntry: return "ROOT_ENTRY";
        case ReachabilityReason::RootExplicit: return "ROOT_EXPLICIT";
        case ReachabilityReason::RootPublicApi: return "ROOT_PUBLIC_API";
        case ReachabilityReason::RootTargetAbi: return "ROOT_TARGET_ABI";
        case ReachabilityReason::ContinuationAbi: return "CONTINUATION_ABI";
        case ReachabilityReason::DirectCall: return "DIRECT_CALL";
        case ReachabilityReason::ActorSpawn: return "ACTOR_SPAWN";
        case ReachabilityReason::ClassReference: return "CLASS_REFERENCE";
        case ReachabilityReason::ActorBody: return "ACTOR_BODY";
        case ReachabilityReason::ClassInitializer: return "CLASS_INITIALIZER";
        case ReachabilityReason::ExportedOverloadAbi: return "EXPORTED_OVERLOAD_ABI";
    }
    throw std::runtime_error("unknown native reachability reason");
}

struct ReachabilityResult {
    std::vector<std::string> roots;
    std::vector<std::string> retained_symbols;
    std::vector<std::string> removed_symbols;
    std::vector<ReachabilityRootReason> root_reasons{};
    std::vector<ReachabilityEdge> edges{};
};

enum class RuntimeFeature {
    ActorNativeRuntime,
    OwnedI64Region,
    IntArrayRuntime
};

struct RuntimeFeatureSet {
    std::set<RuntimeFeature> features;

    bool contains(RuntimeFeature feature) const {
        return features.find(feature) != features.end();
    }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (RuntimeFeature feature : features) {
            switch (feature) {
                case RuntimeFeature::ActorNativeRuntime:
                    out.push_back("actor-native-runtime");
                    break;
                case RuntimeFeature::OwnedI64Region:
                    out.push_back("owned-i64-region");
                    break;
                case RuntimeFeature::IntArrayRuntime:
                    out.push_back("int-array-runtime");
                    break;
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool operator==(const RuntimeFeatureSet& other) const {
        return features == other.features;
    }
    bool operator!=(const RuntimeFeatureSet& other) const {
        return !(*this == other);
    }
};

inline void validate_reachability_result(const ReachabilityResult& result);

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
        const std::string slot =
                native_function_slot(expr->name, expr->arguments.size());
        if (functions.find(slot) == functions.end()) {
            throw std::runtime_error(
                    "native reachability unresolved function call: " + slot);
        }
        function_dependencies.insert(slot);
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
inline RuntimeFeatureSet analyze_runtime_features(
        const Program& program,
        bool wasm32_target = false,
        bool cortex_m_target = false) {
    (void)cortex_m_target;
    RuntimeFeatureSet result;

    if (!wasm32_target && !program.actors.empty())
        result.features.insert(RuntimeFeature::ActorNativeRuntime);

    for (const auto& fn : program.functions) {
        for (const auto& local : fn.locals) {
            if (local.heap_owned)
                result.features.insert(RuntimeFeature::OwnedI64Region);
            if (local.int_array) {
                result.features.insert(RuntimeFeature::IntArrayRuntime);
                continue;
            }
            if (!local.new_class || local.class_name.empty()) continue;
            const auto klass = std::find_if(
                    program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& candidate) {
                        return candidate.name == local.class_name;
                    });
            if (klass == program.classes.end())
                throw std::runtime_error(
                        "native runtime feature analysis references unknown class: " +
                        local.class_name);
            if (std::any_of(
                    klass->fields.begin(), klass->fields.end(),
                    [](const ClassField& field) { return field.int_array; }))
                result.features.insert(RuntimeFeature::IntArrayRuntime);
        }
    }

    return result;
}

inline void validate_runtime_features(
        const Program& program,
        bool wasm32_target,
        bool cortex_m_target,
        const RuntimeFeatureSet& expected) {
    const RuntimeFeatureSet actual =
            analyze_runtime_features(program, wasm32_target, cortex_m_target);
    if (actual != expected) {
        throw std::runtime_error(
                "native runtime feature report drift: expected/actual feature sets differ");
    }
}

inline ReachabilityResult analyze_reachability(
        const Program& program,
        const std::vector<std::string>& explicit_function_roots,
        bool preserve_exported_api,
        const std::vector<std::string>& explicit_actor_roots = {},
        const std::vector<std::string>& explicit_class_roots = {}) {
    std::map<std::string, const Function*> functions;
    std::map<std::string, const ActorDecl*> actors;
    std::map<std::string, const ClassDecl*> classes;
    std::map<std::string, const CooperativeScalarLoop*> cooperative_loops;
    std::set<std::string> semantic_names;
    std::map<std::string, std::size_t> function_family_sizes;
    std::set<std::string> function_names;

    for (const auto& fn : program.functions) {
        const std::string slot =
                native_function_slot(fn.name, fn.parameters.size());
        if (!functions.emplace(slot, &fn).second)
            throw std::runtime_error(
                    "native reachability duplicate function name/arity: " + slot);
        ++function_family_sizes[fn.name];
        function_names.insert(fn.name);
    }
    semantic_names.insert(function_names.begin(), function_names.end());
    for (const auto& actor : program.actors) {
        if (!actors.emplace(actor.name, &actor).second)
            throw std::runtime_error("native reachability rejects duplicate actor name: " + actor.name);
        if (!semantic_names.insert(actor.name).second)
            throw std::runtime_error("native reachability semantic symbol collision: " + actor.name);
    }
    for (const auto& klass : program.classes) {
        if (!classes.emplace(klass.name, &klass).second)
            throw std::runtime_error("native reachability rejects duplicate class name: " + klass.name);
        if (!semantic_names.insert(klass.name).second)
            throw std::runtime_error("native reachability semantic symbol collision: " + klass.name);
    }
    for (const auto& loop : program.cooperative_loops) {
        if (!cooperative_loops.emplace(loop.name, &loop).second ||
                !semantic_names.insert(loop.name).second)
            throw std::runtime_error(
                    "native reachability cooperative-loop symbol collision: " + loop.name);
    }

    std::deque<std::pair<char, std::string>> work;
    std::set<std::string> retained_functions;
    std::set<std::string> retained_actors;
    std::set<std::string> retained_classes;
    std::set<std::string> root_symbols;

    const auto function_semantic_name =
            [&](const std::string& slot, const Function& fn) {
        return function_family_sizes.at(fn.name) > 1 ? slot : fn.name;
    };
    const auto resolve_function_root = [&](const std::string& requested) {
        const auto exact = functions.find(requested);
        if (exact != functions.end()) return requested;
        std::string match;
        for (const auto& [slot, fn] : functions) {
            if (fn->name != requested) continue;
            if (!match.empty())
                throw std::runtime_error(
                        "native reachability ambiguous function root: " + requested);
            match = slot;
        }
        if (match.empty())
            throw std::runtime_error(
                    "native reachability root/function not found: " + requested);
        return match;
    };

    const auto mark_function = [&](const std::string& name) {
        const auto found = functions.find(name);
        if (found == functions.end())
            throw std::runtime_error(
                    "native reachability root/function not found: " + name);
        if (retained_functions.insert(name).second)
            work.emplace_back('f', name);

        // LLVM source ABI suffixing depends on overload-family cardinality.
        // If a retained source-public overload survives, preserve its whole
        // family so tree shaking cannot silently rename that exported symbol.
        if (found->second->exported &&
                function_family_sizes.at(found->second->name) > 1) {
            for (const auto& [slot, candidate] : functions) {
                if (candidate->name != found->second->name) continue;
                if (retained_functions.insert(slot).second)
                    work.emplace_back('f', slot);
            }
        }
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

    for (const auto& root : explicit_function_roots) {
        const std::string slot = resolve_function_root(root);
        mark_function(slot);
        root_symbols.insert(reachability_detail::qualify(
                program, function_semantic_name(slot, *functions.at(slot))));
    }
    for (const auto& root : explicit_actor_roots) {
        mark_actor(root);
        root_symbols.insert(reachability_detail::qualify(program, root));
    }
    for (const auto& root : explicit_class_roots) {
        mark_class(root);
        root_symbols.insert(reachability_detail::qualify(program, root));
    }
    if (preserve_exported_api) {
        for (const auto& [slot, fn] : functions) {
            if (!fn->exported) continue;
            mark_function(slot);
            root_symbols.insert(reachability_detail::qualify(
                    program, function_semantic_name(slot, *fn)));
        }
    }

    // The legacy scalar-while form is also a host-driven continuation: when
    // admitted, codegen replaces the source function with externally callable
    // init/resume/cancel entrypoints. It is therefore a semantic host root even
    // when the source declaration is private and there is no ordinary caller.
    for (const auto& [slot, fn] : functions) {
        if (!fn->scalar_while) continue;
        mark_function(slot);
        root_symbols.insert(reachability_detail::qualify(
                program, function_semantic_name(slot, *fn)));
    }

    // Cooperative scalar loops lower to externally callable init/resume/cancel
    // symbols. Until a host-import contract can prove otherwise, every such
    // loop is an explicit semantic root.
    for (const auto& [name, loop] : cooperative_loops) {
        (void)loop;
        root_symbols.insert(reachability_detail::qualify(program, name));
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

    for (const auto& [slot, fn] : functions) {
        const std::string semantic =
                function_semantic_name(slot, *fn);
        (retained_functions.count(slot) ? retained : removed)
                .insert(reachability_detail::qualify(program, semantic));
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
    for (const auto& [name, loop] : cooperative_loops) {
        (void)loop;
        retained.insert(reachability_detail::qualify(program, name));
    }

    ReachabilityResult result{
        reachability_detail::sorted(root_symbols),
        reachability_detail::sorted(retained),
        reachability_detail::sorted(removed)};
    validate_reachability_result(result);
    return result;
}



inline Program prune_program(Program program, const ReachabilityResult& result) {
    validate_reachability_result(result);

    std::set<std::string> retained(
            result.retained_symbols.begin(), result.retained_symbols.end());
    std::set<std::string> removed(
            result.removed_symbols.begin(), result.removed_symbols.end());
    std::set<std::string> source_symbols;
    std::map<std::string, std::size_t> function_family_sizes;
    for (const auto& fn : program.functions)
        ++function_family_sizes[fn.name];

    const auto function_semantic_name = [&](const Function& fn) {
        return function_family_sizes.at(fn.name) > 1
                ? native_function_slot(fn.name, fn.parameters.size())
                : fn.name;
    };

    const auto register_symbol = [&](const std::string& name) {
        const std::string symbol = reachability_detail::qualify(program, name);
        if (!source_symbols.insert(symbol).second)
            throw std::runtime_error(
                    "native tree shaking source semantic symbol collision: " + symbol);
        const bool keep = retained.count(symbol) != 0;
        const bool drop = removed.count(symbol) != 0;
        if (keep == drop)
            throw std::runtime_error(
                    "native tree shaking requires exactly one classification for: " + symbol);
    };

    for (const auto& fn : program.functions)
        register_symbol(function_semantic_name(fn));
    for (const auto& actor : program.actors) register_symbol(actor.name);
    for (const auto& klass : program.classes) register_symbol(klass.name);
    for (const auto& loop : program.cooperative_loops) register_symbol(loop.name);

    if (retained.size() + removed.size() != source_symbols.size())
        throw std::runtime_error(
                "native tree shaking manifest contains symbols outside the source program");
    for (const auto& symbol : retained)
        if (!source_symbols.count(symbol))
            throw std::runtime_error(
                    "native tree shaking retained unknown source symbol: " + symbol);
    for (const auto& symbol : removed)
        if (!source_symbols.count(symbol))
            throw std::runtime_error(
                    "native tree shaking removed unknown source symbol: " + symbol);

    const auto should_remove = [&](const auto& declaration) {
        return retained.count(
                reachability_detail::qualify(program, declaration.name)) == 0;
    };

    program.functions.erase(
            std::remove_if(
                    program.functions.begin(),
                    program.functions.end(),
                    [&](const Function& fn) {
                        return retained.count(reachability_detail::qualify(
                                program, function_semantic_name(fn))) == 0;
                    }),
            program.functions.end());
    program.actors.erase(
            std::remove_if(program.actors.begin(), program.actors.end(), should_remove),
            program.actors.end());
    program.classes.erase(
            std::remove_if(program.classes.begin(), program.classes.end(), should_remove),
            program.classes.end());
    program.cooperative_loops.erase(
            std::remove_if(program.cooperative_loops.begin(),
                           program.cooperative_loops.end(), should_remove),
            program.cooperative_loops.end());

    return program;
}


struct TreeShakenProgram {
    Program program;
    ReachabilityResult reachability;
    RuntimeFeatureSet runtime_features;
};

/**
 * Production tree-shaking entry point. Analysis and deletion are deliberately
 * coupled so a serialized/stale manifest is never accepted as deletion
 * authority for a different compiler Program.
 */
inline TreeShakenProgram tree_shake_program(
        Program program,
        const std::vector<std::string>& explicit_function_roots,
        bool preserve_exported_api,
        const std::vector<std::string>& explicit_actor_roots = {},
        const std::vector<std::string>& explicit_class_roots = {},
        bool wasm32_target = false,
        bool cortex_m_target = false) {
    ReachabilityResult reachability = analyze_reachability(
            program,
            explicit_function_roots,
            preserve_exported_api,
            explicit_actor_roots,
            explicit_class_roots);
    program = prune_program(std::move(program), reachability);
    RuntimeFeatureSet runtime_features =
            analyze_runtime_features(program, wasm32_target, cortex_m_target);
    return TreeShakenProgram{
            std::move(program),
            std::move(reachability),
            std::move(runtime_features)};
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

inline void validate_reachability_symbol(std::string_view value) {
    if (value.empty())
        throw std::runtime_error("native reachability symbol cannot be empty");

    std::size_t scalar_count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::uint32_t code_point = 0;
        std::size_t width = 0;

        if (lead <= 0x7f) {
            code_point = lead;
            width = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            code_point = lead & 0x1fu;
            width = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            code_point = lead & 0x0fu;
            width = 3;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            code_point = lead & 0x07u;
            width = 4;
        } else {
            throw std::runtime_error("native reachability symbol is not valid UTF-8");
        }

        if (i + width > value.size())
            throw std::runtime_error("native reachability symbol is truncated UTF-8");

        for (std::size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0u) != 0x80u)
                throw std::runtime_error("native reachability symbol has invalid UTF-8 continuation");
            code_point = (code_point << 6) | (continuation & 0x3fu);
        }

        if ((width == 2 && code_point < 0x80u) ||
            (width == 3 && code_point < 0x800u) ||
            (width == 4 && code_point < 0x10000u) ||
            (code_point >= 0xd800u && code_point <= 0xdfffu) ||
            code_point > 0x10ffffu)
            throw std::runtime_error("native reachability symbol has non-scalar UTF-8");

        if (code_point <= 0x1fu || code_point == 0x7fu)
            throw std::runtime_error("native reachability symbol contains a control character");
        if (++scalar_count > 1024)
            throw std::runtime_error("native reachability symbol exceeds 1024 Unicode scalars");

        i += width;
    }
}

inline bool reachability_strictly_sorted_unique(
        const std::vector<std::string>& values) {
    for (std::size_t i = 1; i < values.size(); ++i)
        if (!(values[i - 1] < values[i])) return false;
    return true;
}

inline void validate_reachability_result(const ReachabilityResult& result) {
    for (const auto& symbol : result.roots) validate_reachability_symbol(symbol);
    for (const auto& symbol : result.retained_symbols) validate_reachability_symbol(symbol);
    for (const auto& symbol : result.removed_symbols) validate_reachability_symbol(symbol);

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
