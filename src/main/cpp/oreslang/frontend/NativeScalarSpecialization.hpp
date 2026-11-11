#pragma once

#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace oreslang {

// An independently reconstructed, bounded pre-emission dependency proof for
// the already-supported concrete-i64 specialization slice. This is not trait
// witness selection or a source-authored certificate. All generic declarations
// are emission roots because this backend emits their bodies even when unused.
struct NativeI64SpecializationClosureEvidence {
    std::size_t scanned_nodes;
    std::size_t dependency_edges;
    std::size_t estimated_code_bytes;
    std::size_t concrete_symbols;
};

inline NativeI64SpecializationClosureEvidence check_native_i64_specialization_closure(
        const Program& program,
        const std::unordered_map<std::string, std::string>& selected) {
    if (selected.empty()) return {0, 0, 0, 0};
    constexpr std::size_t kMaxScanNodes = 131072;
    constexpr std::size_t kMaxEdges = 65536;
    constexpr std::size_t kMaxEstimatedBytes = 8 * 1024 * 1024;
    std::unordered_map<std::string, std::unordered_set<std::string>> graph;
    std::size_t scanned = 0, edges = 0, estimated = 0;

    const auto scan = [&](const Function& fn, bool emitted_generic) {
        std::vector<const Expression*> expressions;
        std::vector<const Statement*> statements;
        const auto push = [&](const std::unique_ptr<Expression>& e) {
            if (e) expressions.push_back(e.get());
        };
        const auto push_local = [&](const LocalBinding& local) {
            push(local.initializer);
            for (const auto& arg : local.constructor_arguments) push(arg);
        };
        const auto push_statements = [&](const std::vector<Statement>& group) {
            for (const auto& statement : group) statements.push_back(&statement);
        };

        push(fn.result);
        push(fn.caught_throw_value);
        for (const auto& local : fn.locals) push_local(local);
        for (const auto& guard : fn.early_returns) {
            push(guard.condition);
            push(guard.lhs);
            push(guard.rhs);
            push(guard.result);
        }
        for (const auto& send : fn.actor_sends) push(send.payload);
        for (const auto& write : fn.actor_field_writes) push(write.expression);
        for (const auto& task : fn.source_tasks) push(task.result);
        push_statements(fn.statements);
        push_statements(fn.caught_throw_defers);

        const auto caller = native_function_slot(fn.name, fn.parameters.size());
        const bool generic_body = emitted_generic && selected.find(caller) != selected.end();
        std::size_t fn_nodes = 0;
        std::unordered_set<std::string> outgoing;
        while (!expressions.empty() || !statements.empty()) {
            if (++scanned > kMaxScanNodes)
                throw std::runtime_error("native AOT specialization AST scan budget exceeded");
            ++fn_nodes;
            if (!statements.empty()) {
                const auto* statement = statements.back();
                statements.pop_back();
                push(statement->expression);
                push(statement->condition);
                push(statement->condition_lhs);
                push(statement->condition_rhs);
                push(statement->loop_step);
                push(statement->array_commit_length);
                for (const auto& value : statement->array_commit_values) push(value);
                if (statement->branch_local) push_local(*statement->branch_local);
                push_statements(statement->then_statements);
                push_statements(statement->else_statements);
                continue;
            }
            const auto* expr = expressions.back();
            expressions.pop_back();
            if (generic_body && expr->kind == Expression::Kind::Call) {
                const auto key = native_function_slot(expr->name, expr->arguments.size());
                if (selected.find(key) != selected.end() && outgoing.insert(key).second) {
                    if (++edges > kMaxEdges)
                        throw std::runtime_error("native AOT specialization edge budget exceeded");
                }
            }
            push(expr->left);
            push(expr->right);
            push(expr->condition);
            push(expr->condition_lhs);
            push(expr->condition_rhs);
            push(expr->when_true);
            push(expr->when_false);
            for (const auto& arg : expr->arguments) push(arg);
        }
        if (!generic_body) return;
        // Conservative pre-emission estimate, not a measured final binary
        // byte count. Limit growth before creating the native LLVM module.
        if (estimated > kMaxEstimatedBytes - 256 ||
            fn_nodes > (kMaxEstimatedBytes - estimated - 256) / 64)
            throw std::runtime_error("native AOT specialization byte estimate exceeded");
        estimated += 256 + fn_nodes * 64;
        if (!graph.emplace(caller, std::move(outgoing)).second)
            throw std::runtime_error("duplicate native AOT specialization graph node");
    };

    for (const auto& fn : program.functions) scan(fn, true);
    for (const auto& actor : program.actors) scan(actor.on_message, false);

    // Traverse each emitted generic as a root. Recursive calls and diamonds
    // terminate because every concrete specialization is visited once.
    std::vector<std::string> pending;
    for (const auto& pair : selected) pending.push_back(pair.first);
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        auto key = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(key).second) continue;
        const auto node = graph.find(key);
        if (node == graph.end())
            throw std::runtime_error("missing emitted native AOT specialization node");
        for (const auto& dependency : node->second) {
            if (selected.find(dependency) == selected.end())
                throw std::runtime_error("missing concrete native AOT specialization dependency");
            if (visited.find(dependency) == visited.end()) pending.push_back(dependency);
        }
    }
    if (visited.size() != selected.size())
        throw std::runtime_error("incomplete native AOT specialization closure");
    return {scanned, edges, estimated, visited.size()};
}

// Closed, compiler-derived stage-1 native generic ABI. The native scalar type
// checker currently admits only concrete int for generic type arguments;
// it does NOT admit arbitrary traits, nominal generics, or runtime reification.
// The suffix cannot collide with a lexical source identifier ('$' is reserved).
//
// This plan is deliberately rebuilt by the emitter from the post-validation
// Program rather than accepted as source-authored metadata. One declaration
// gets exactly one concrete i64 specialization, reused by all matching calls.
inline std::unordered_map<std::string, std::string>
freeze_native_i64_specializations(const Program& program) {
    constexpr std::size_t kMaxSpecializations = 256;
    constexpr std::size_t kMaxGenericParameters = 32;
    std::unordered_map<std::string, std::string> selected;
    std::unordered_set<std::string> concrete_symbols;
    std::unordered_set<std::string> declaration_selectors;
    for (const auto& fn : program.functions) {
        // Freeze all selector identities, not just generic ones. Generated
        // compiler IR may otherwise define both a plain and generic function
        // with the same call selector, and codegen cannot preserve both.
        const std::string selector = native_function_slot(fn.name, fn.parameters.size());
        if (!declaration_selectors.insert(selector).second)
            throw std::runtime_error("duplicate compiler-owned native function selector");
        // Reject stale or forged generic ABI metadata even when a function
        // claims to be non-generic. Otherwise codegen would silently erase a
        // GenericType parameter to i64 without a frozen specialization.
        if (fn.generic_parameters.empty()) {
            for (const auto& param : fn.parameters) {
                if (!param.generic_type.empty())
                    throw std::runtime_error(
                        "undeclared native AOT generic parameter ABI");
            }
            continue;
        }
        if (selected.size() == kMaxSpecializations)
            throw std::runtime_error("native AOT generic specialization count exceeded");
        if (!fn.method_owner.empty() || fn.generic_parameters.size() > kMaxGenericParameters)
            throw std::runtime_error("native AOT generic method or arity not supported");
        std::unordered_set<std::string> known;
        const auto canonical = [](const std::string& name) {
            if (name.empty() || name.size() > 128) return false;
            const auto first = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            };
            const auto rest = [&](char c) {
                return first(c) || (c >= '0' && c <= '9');
            };
            if (!first(name.front())) return false;
            for (char c : name) if (!rest(c)) return false;
            return true;
        };
        // Check the declaration shape rather than trusting a previously
        // serialized "specialization approved" boolean.
        for (const auto& param : fn.generic_parameters) {
            if (!canonical(param.name) || known.find(param.name) != known.end())
                throw std::runtime_error("invalid native AOT generic parameter identity");
            if (!param.default_type.empty() && param.default_type != "int" &&
                    known.find(param.default_type) == known.end())
                throw std::runtime_error("invalid native AOT generic default dependency");
            known.insert(param.name);
        }
        for (const auto& param : fn.parameters) {
            if (!param.generic_type.empty() &&
                    (known.find(param.generic_type) == known.end() ||
                     param.scalar_type != ScalarType::I64 ||
                     !param.pending_type_alias.empty() ||
                     !param.class_name.empty() || !param.sum_type.empty()))
                throw std::runtime_error("unresolved native AOT generic parameter ABI");
        }
        // Only int is a concrete generic type in the existing native checker.
        // Encoding the complete ordered tuple makes this a real fixed symbol
        // rather than a runtime generic name/registry lookup.
        std::string suffix = "$atto_g" + std::to_string(fn.generic_parameters.size());
        for (std::size_t i = 0; i < fn.generic_parameters.size(); ++i)
            suffix += "$int";
        if (!selected.emplace(selector, suffix).second)
            throw std::runtime_error("duplicate native AOT generic specialization identity");
        if (!concrete_symbols.insert(selector + suffix).second)
            throw std::runtime_error("duplicate native AOT specialized symbol");
    }
    (void)check_native_i64_specialization_closure(program, selected);
    return selected;
}

} // namespace oreslang
