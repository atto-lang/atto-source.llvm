#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Host/front-end-only namespace identity model. NOT native guest source
// admission, compiler linking or LLVM codegen. Trusted source IDs, owners and
// alias pins must come from a future verified manifest/lockfile graph.
namespace oreslang::frontend {

struct PackageNamespaceKey {
    std::string package_instance_id;
    std::string name;

    bool operator<(const PackageNamespaceKey& other) const {
        return std::tie(package_instance_id, name)
            < std::tie(other.package_instance_id, other.name);
    }
};

struct PackageNamespaceExport {
    std::string source_unit_id;
    std::string name;
    std::size_t arity = 0;
};

struct PackageNamespaceGroup {
    std::set<std::string> source_units;
    std::vector<PackageNamespaceExport> exports;
};

struct NamespaceOwnedSource {
    std::string source_unit_id;
    std::string package_instance_id;
    const Program* program = nullptr;
};

struct NamespaceImportRequest {
    std::string importer_source_unit_id;
    std::string package_alias;
    std::string namespace_name;
};

using PackageAliasPins =
    std::map<std::string, std::map<std::string, std::string>>;

struct PackageNamespaceGraph {
    std::map<PackageNamespaceKey, PackageNamespaceGroup> namespaces;
    std::map<std::pair<std::string, std::string>, PackageNamespaceKey> imports;
};

inline bool valid_flat_namespace_name(const std::string& name) {
    if (name.empty() || name == "__root__" || name == "__proto__"
            || name == "constructor" || name == "prototype") return false;
    const auto start = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto rest = [&](unsigned char c) {
        return start(c) || (c >= '0' && c <= '9');
    };
    if (!start(static_cast<unsigned char>(name.front()))) return false;
    return std::all_of(name.begin() + 1, name.end(), [&](char ch) {
        return rest(static_cast<unsigned char>(ch));
    });
}

// Until an effect-aware cross-unit body checker exists, only scalar,
 // call-free expression trees can enter the host namespace export inventory.
 // An apparently scalar result can still hide Console/Call/borrow/nominal
 // operations in a nested expression or local initializer.
// Bounded admission prevents a malformed/host-injected scalar AST from
// exhausting the linker stack before a verified native lowering exists.
inline constexpr std::size_t kMaxNamespaceScalarExpressionDepth = 128;
inline bool namespace_pure_scalar_expression_at_depth(
        const Expression* expression, std::size_t depth) {
    if (!expression) return true;
    if (depth >= kMaxNamespaceScalarExpressionDepth) return false;
    // A scalar tag is not proof that a forged/unresolved AST is well formed.
    // Require complete operands, and refuse surplus children which could hide
    // unvalidated evaluation paths. The legacy equality-ternary representation
    // is admitted only when BOTH comparison operands are present.
    const auto no_ternary_children = [&] {
        return !expression->condition && !expression->condition_lhs
            && !expression->condition_rhs && !expression->when_true
            && !expression->when_false;
    };
    // Scalar-only export admission must validate the type of each AST edge,
    // not merely that every node has *some* scalar resolved_type.
    const auto type_is = [](const Expression* node, ScalarType type) {
        return node && node->resolved_type == type;
    };
    switch (expression->kind) {
        case Expression::Kind::Integer:
            if (expression->resolved_type != ScalarType::I64
                    || expression->left || expression->right || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Boolean:
            if (expression->resolved_type != ScalarType::Bool
                    || expression->left || expression->right || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Variable:
            if (!valid_flat_namespace_name(expression->name)
                    || expression->left || expression->right || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Negate:
            if (!type_is(expression->left.get(), ScalarType::I64)
                    || expression->resolved_type != ScalarType::I64
                    || expression->right || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::LogicalNot:
            if (!type_is(expression->left.get(), ScalarType::Bool)
                    || expression->resolved_type != ScalarType::Bool
                    || expression->right || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
        case Expression::Kind::Remainder:
            if (!type_is(expression->left.get(), ScalarType::I64)
                    || !type_is(expression->right.get(), ScalarType::I64)
                    || expression->resolved_type != ScalarType::I64
                    || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Equal:
        case Expression::Kind::NotEqual:
            if (!expression->left || !expression->right
                    || expression->left->resolved_type != expression->right->resolved_type
                    || expression->resolved_type != ScalarType::Bool
                    || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Less:
        case Expression::Kind::LessEqual:
        case Expression::Kind::Greater:
        case Expression::Kind::GreaterEqual:
            if (!type_is(expression->left.get(), ScalarType::I64)
                    || !type_is(expression->right.get(), ScalarType::I64)
                    || expression->resolved_type != ScalarType::Bool
                    || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::LogicalAnd:
        case Expression::Kind::LogicalOr:
        case Expression::Kind::LogicalXor:
            if (!type_is(expression->left.get(), ScalarType::Bool)
                    || !type_is(expression->right.get(), ScalarType::Bool)
                    || expression->resolved_type != ScalarType::Bool
                    || !no_ternary_children())
                return false;
            break;
        case Expression::Kind::Conditional:
            if (expression->left || expression->right
                    || !expression->when_true || !expression->when_false
                    || expression->when_true->resolved_type != expression->resolved_type
                    || expression->when_false->resolved_type != expression->resolved_type)
                return false;
            if (expression->condition) {
                if (expression->condition_lhs || expression->condition_rhs
                        || expression->condition->resolved_type != ScalarType::Bool)
                    return false;
            } else if (!expression->condition_lhs || !expression->condition_rhs
                    || expression->condition_lhs->resolved_type
                        != expression->condition_rhs->resolved_type) {
                return false;
            }
            break;
        default:
            return false;
    }
    if (expression->resolved_type != ScalarType::I64
            && expression->resolved_type != ScalarType::Bool) return false;
    if (!expression->resolved_class.empty() || !expression->sum_type.empty()
            || !expression->arguments.empty() || !expression->argument_names.empty()
            || !expression->type_arguments.empty() || expression->type_arguments_present)
        return false;
    return namespace_pure_scalar_expression_at_depth(expression->left.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->right.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->condition.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->condition_lhs.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->condition_rhs.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->when_true.get(), depth + 1)
        && namespace_pure_scalar_expression_at_depth(expression->when_false.get(), depth + 1);
}

inline bool namespace_pure_scalar_expression(const Expression* expression) {
    return namespace_pure_scalar_expression_at_depth(expression, 0);
}

// This API cannot invent package ownership from a namespace header, pathname,
// compilation directory or globally shared namespace. It deliberately deals
// only with native scalar public function signatures; richer signatures and
// runtime binding remain blocked until actual checked lowering is available.
inline PackageNamespaceGraph link_package_namespaces(
        const std::vector<NamespaceOwnedSource>& sources,
        const PackageAliasPins& pinned_aliases,
        const std::vector<NamespaceImportRequest>& imports) {
    PackageNamespaceGraph result;
    std::map<std::string, std::string> owner_by_source;
    std::set<std::string> verified_instances;
    std::map<PackageNamespaceKey, std::set<std::pair<std::string, std::size_t>>> signatures;

    for (const auto& unit : sources) {
        if (unit.source_unit_id.empty() || unit.package_instance_id.empty()
                || unit.program == nullptr
                || !owner_by_source.emplace(unit.source_unit_id, unit.package_instance_id).second)
            throw std::invalid_argument("namespace source requires unique verified source/package ownership");
        verified_instances.insert(unit.package_instance_id);

        const Program& program = *unit.program;
        if (program.namespace_name.empty()) continue;
        if (!valid_flat_namespace_name(program.namespace_name))
            throw std::invalid_argument("invalid flat source namespace header");
        // A module wrapper and a file namespace are distinct scopes. Never
        // manufacture an empty importable namespace from a module-owned file.
        if (!program.module.empty())
            throw std::invalid_argument("module wrapper cannot declare a file namespace");
        if (!program.imports.empty())
            throw std::invalid_argument("native namespace unresolved module imports require checked dependency lowering");

        // Reject unlowered top-level declarations before registering the
        // namespace; no nominal/constraint/loop ABI can enter this scalar graph.
        if (!program.classes.empty() || !program.actors.empty()
                || !program.type_only_declarations.empty()
                || !program.type_constraint_descriptors.empty()
                || !program.contracts.empty() || !program.conforms.empty()
                || !program.sum_types.empty()
                || !program.generators.empty()
                || !program.cooperative_loops.empty())
            throw std::invalid_argument("native namespace non-scalar declarations need checked ABI lowering");

        const PackageNamespaceKey key{unit.package_instance_id, program.namespace_name};
        auto& group = result.namespaces[key];
        group.source_units.insert(unit.source_unit_id);

        for (const auto& fn : program.functions) {
            if (!fn.exported) continue;
            // A linker-owned implementation is private even if a malformed AST
            // sets the public bit; public ABI names must be flat identifiers.
            if (fn.native_internal)
                throw std::invalid_argument("private native implementation cannot be a namespace export");
            if (!valid_flat_namespace_name(fn.name))
                throw std::invalid_argument("invalid public namespace export name");
            if (fn.name == "init")
                throw std::invalid_argument("native namespace init lifecycle requires checked lowering");
            if (!fn.statements.empty() || !fn.early_returns.empty())
                throw std::invalid_argument("native namespace body control flow requires checked lowering");
            if (!namespace_pure_scalar_expression(fn.result.get()))
                throw std::invalid_argument("native namespace body expression requires checked effect lowering");
            if (!fn.generic_parameters.empty() || !fn.method_owner.empty())
                throw std::invalid_argument("native namespace generic/method exports require checked lowering");
            // An arity-only export cannot hide a borrowed, nominal, sum,
            // unresolved-alias, String or void ABI behind a scalar signature.
            // Bool and i64 value parameters/results are the only admitted
            // typed shapes in this host-only inventory.
            if (fn.async_source_i64)
                throw std::invalid_argument("native namespace async export requires checked ABI lowering");
            // The scalar return type alone cannot certify an executable
            // suspension boundary. Await, task and Future pipeline metadata
            // require a separately checked cross-unit continuation ABI.
            if (!fn.source_tasks.empty() || !fn.future_pipelines.empty()
                    || !fn.ready_await_actor.empty() || !fn.done_await_actor.empty()
                    || fn.ready_await_offset != 0 || fn.done_await_offset != 0
                    || fn.ready_await_form != NativeAwaitForm::None
                    || !fn.ready_future_name.empty() || !fn.ready_future_actor.empty())
                throw std::invalid_argument("native namespace suspension export requires checked ABI lowering");
            // Await continuations can also be encoded on local bindings,
            // even when no producer or pipeline metadata is present. The
            // scalar export inventory cannot erase these scheduler effects.
            for (const auto& local : fn.locals) {
                if (!namespace_pure_scalar_expression(local.initializer.get()))
                    throw std::invalid_argument("native namespace local initializer requires checked effect lowering");
                if (!local.task_await_name.empty() || !local.future_await_name.empty())
                    throw std::invalid_argument("native namespace suspension export requires checked ABI lowering");
            }
            // The scalar signature alone also cannot certify a function's
            // activation frame or local ownership. The experimental scalar
            // loop has resumable state; owned arrays, nominal/record views,
            // constructor/move metadata and unresolved aliases need checked
            // cross-unit lifetime and representation lowering.
            if (fn.scalar_while)
                throw std::invalid_argument("native namespace scalar loop frame requires checked ABI lowering");
            for (const auto& local : fn.locals) {
                if (local.heap_owned || local.int_array || local.record_view
                        || !local.class_name.empty() || local.new_class || local.move_class
                        || !local.move_source.empty() || local.field_mutable
                        || !local.array_record_class.empty()
                        || !local.constructor_arguments.empty()
                        || !local.pending_type_alias.empty() || !local.sum_type.empty()
                        || (local.scalar_type != ScalarType::I64 && local.scalar_type != ScalarType::Bool)
                        || (local.array_element_type != ScalarType::I64
                            && local.array_element_type != ScalarType::Bool))
                    throw std::invalid_argument("native namespace owned/nominal local requires checked lowering");
            }
            // A (name, arity) inventory has no effect row, actor capability
            // or exception/cleanup ABI. Reject these unlowered boundaries
            // rather than silently exporting an apparently pure scalar call.
            // Even an explicitly empty declared row requires validation by
            // the future effect-aware cross-unit linker.
            if (fn.has_declared_effects || fn.declared_effects.bits != 0)
                throw std::invalid_argument("native namespace effect row requires checked ABI lowering");
            if (!fn.actor_spawns.empty() || !fn.actor_sends.empty()
                    || !fn.actor_stops.empty() || !fn.actor_actions.empty()
                    || !fn.actor_field_writes.empty()
                    || fn.caught_throw_value != nullptr
                    || !fn.catch_binding.empty() || !fn.caught_throw_defers.empty())
                throw std::invalid_argument("native namespace actor/exception effects require checked lowering");
            if (fn.returns_void
                    || (fn.return_type != ScalarType::I64 && fn.return_type != ScalarType::Bool)
                    || !fn.return_class.empty() || !fn.return_sum.empty()
                    || !fn.pending_return_type_alias.empty())
                throw std::invalid_argument("native namespace non-scalar return signature requires checked ABI lowering");
            for (const auto& param : fn.parameters) {
                if (param.mode != Parameter::Mode::Value
                        || (param.scalar_type != ScalarType::I64 && param.scalar_type != ScalarType::Bool)
                        || !param.class_name.empty() || !param.generic_type.empty()
                        || !param.pending_type_alias.empty() || !param.sum_type.empty())
                    throw std::invalid_argument("native namespace non-scalar parameter signature requires checked ABI lowering");
            }
            const auto signature = std::make_pair(fn.name, fn.parameters.size());
            if (!signatures[key].insert(signature).second)
                throw std::invalid_argument("duplicate public namespace function/arity in same package instance");
            group.exports.push_back({unit.source_unit_id, fn.name, fn.parameters.size()});
        }
    }

    for (auto& entry : result.namespaces) {
        auto& exports = entry.second.exports;
        std::sort(exports.begin(), exports.end(), [](const auto& a, const auto& b) {
            return std::tie(a.name, a.arity, a.source_unit_id)
                < std::tie(b.name, b.arity, b.source_unit_id);
        });
    }

    for (const auto& imported : imports) {
        const auto from = owner_by_source.find(imported.importer_source_unit_id);
        if (from == owner_by_source.end())
            throw std::invalid_argument("namespace importer lacks verified package ownership");
        if (!valid_flat_namespace_name(imported.package_alias)
                || !valid_flat_namespace_name(imported.namespace_name))
            throw std::invalid_argument("invalid namespace import alias or selector");

        const auto alias_table = pinned_aliases.find(from->second);
        if (alias_table == pinned_aliases.end()
                || alias_table->second.count(imported.package_alias) == 0)
            throw std::invalid_argument("undeclared pinned package alias for namespace import");
        const std::string& target_package = alias_table->second.at(imported.package_alias);
        if (verified_instances.count(target_package) == 0)
            throw std::invalid_argument("namespace import target lacks verified package inventory");
        PackageNamespaceKey target{target_package, imported.namespace_name};
        if (result.namespaces.count(target) == 0)
            throw std::invalid_argument("namespace absent in selected package; no cross-package fallback");

        if (!result.imports.emplace(
                std::make_pair(imported.importer_source_unit_id, imported.namespace_name),
                std::move(target)).second)
            throw std::invalid_argument("duplicate lexical namespace import binding");
    }
    return result;
}

} // namespace oreslang::frontend
