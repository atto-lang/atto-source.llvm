#pragma once

#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
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
    // Deterministically ordered, closed concrete-selector dependency table.
    // Produced by the compiler's post-validation AST walk, never imported.
    std::map<std::string, std::vector<std::string>> dependencies;
};

inline NativeI64SpecializationClosureEvidence check_native_i64_specialization_closure(
        const Program& program,
        const std::unordered_map<std::string, std::string>& selected) {
    if (selected.empty()) return {0, 0, 0, 0, {}};
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

    // A closed *generic* specialization graph must not consume its
    // computational budget traversing unrelated functions or actor handlers.
    // Nongeneric call sites are validated by the normal typed call checker.
    for (const auto& fn : program.functions) {
        if (selected.find(native_function_slot(fn.name, fn.parameters.size())) != selected.end())
            scan(fn, true);
    }

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
    std::map<std::string, std::vector<std::string>> frozen_dependencies;
    for (const auto& [selector, outgoing] : graph) {
        std::vector<std::string> ordered(outgoing.begin(), outgoing.end());
        std::sort(ordered.begin(), ordered.end());
        if (!frozen_dependencies.emplace(selector, std::move(ordered)).second)
            throw std::runtime_error("duplicate native AOT frozen dependency identity");
    }
    return {scanned, edges, estimated, visited.size(), std::move(frozen_dependencies)};
}

// Closed, compiler-derived stage-1 native generic ABI. The native scalar type
// checker currently admits only concrete int for generic type arguments;
// it does NOT admit arbitrary traits, nominal generics, or runtime reification.
// The suffix cannot collide with a lexical source identifier ('$' is reserved).
//
// This plan is deliberately rebuilt by the emitter from the post-validation
// Program rather than accepted as source-authored metadata. One declaration
// gets exactly one concrete i64 specialization, reused by all matching calls.
// Frozen compiler-owned direct-call records. The tuple is concrete and
// ordered; each record is derived from the admitted Program, never from a
// source-authored manifest or a runtime symbol registry.
// Explicit ABI snapshot; label and parameter loan mode are part of the
// statically checked call contract, even though scalar int/bool values lower
// into the same physical LLVM i64 register.
struct NativeI64ParameterAbi {
    std::string name;
    ScalarType scalar_type;
    Parameter::Mode mode;
    std::string generic_type;
    std::string class_name;
    std::string sum_type;
};

struct NativeI64DirectTarget {
    std::string selector;
    std::string emitted_suffix;
    std::string emitted_full_symbol; // canonical compiler-linker identity
    std::vector<std::string> concrete_types;
    std::size_t lowered_arity;
    std::vector<NativeI64ParameterAbi> parameter_abi;
    ScalarType return_type;
    bool returns_void;
    std::string return_class;
    std::string return_sum;
    bool has_declared_effects;
    FunctionEffects declared_effects;
    bool exported;
    bool native_internal;
};

// The bridge must not silently change borrow modes, argument labels,
// concrete generic identities, effects, visibility or return ABI between the
// frozen typed IR and the LLVM declarations that consume it.
inline void verify_native_i64_direct_target(
        const Function& fn, const NativeI64DirectTarget& target) {
    if (target.selector != native_function_slot(fn.name, fn.parameters.size()) ||
        target.lowered_arity != fn.parameters.size() ||
        target.parameter_abi.size() != fn.parameters.size() ||
        target.concrete_types.size() != fn.generic_parameters.size() ||
        fn.generic_parameters.empty() ||
        !fn.pending_return_type_alias.empty() ||
        !fn.declared_effects.valid() || !target.declared_effects.valid())
        throw std::runtime_error("native AOT frozen direct dispatch ABI mismatch");
    std::string expected_suffix = "$atto_g" + std::to_string(fn.generic_parameters.size());
    for (const auto& type : target.concrete_types) {
        if (type != "int")
            throw std::runtime_error("native AOT frozen direct dispatch ABI mismatch");
        expected_suffix += "$int";
    }
    if (target.emitted_suffix != expected_suffix ||
        target.emitted_full_symbol.size() <= target.emitted_suffix.size() ||
        target.emitted_full_symbol.compare(
            target.emitted_full_symbol.size() - target.emitted_suffix.size(),
            target.emitted_suffix.size(), target.emitted_suffix) != 0 ||
        target.return_type != fn.return_type ||
        target.returns_void != fn.returns_void ||
        target.return_class != fn.return_class ||
        target.return_sum != fn.return_sum ||
        target.has_declared_effects != fn.has_declared_effects ||
        target.declared_effects.bits != fn.declared_effects.bits ||
        target.exported != fn.exported ||
        target.native_internal != fn.native_internal ||
        !fn.c_export_u32_symbol.empty())
        throw std::runtime_error("native AOT frozen direct dispatch ABI mismatch");
    for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        const auto& param = fn.parameters[i];
        const auto& frozen = target.parameter_abi[i];
        if (!param.pending_type_alias.empty() ||
            frozen.name != param.name ||
            frozen.scalar_type != param.scalar_type ||
            frozen.mode != param.mode ||
            frozen.generic_type != param.generic_type ||
            frozen.class_name != param.class_name ||
            frozen.sum_type != param.sum_type)
            throw std::runtime_error("native AOT frozen direct dispatch ABI mismatch");
    }
}

struct NativeI64DispatchImage {
    NativeI64SpecializationClosureEvidence closure;
    std::unordered_map<std::string, NativeI64DirectTarget> direct_targets;

    const NativeI64DirectTarget* find_direct(const std::string& selector) const {
        const auto found = direct_targets.find(selector);
        return found == direct_targets.end() ? nullptr : &found->second;
    }
};

// This selector builder is deliberately internal to the frozen-image path;
// callers must not regard the returned mapping alone as dispatch authority.
inline std::unordered_map<std::string, std::string>
select_native_i64_specializations(const Program& program) {
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
    return selected;
}

// This factory is the production bridge between the checked typed Program,
// its bounded specialization closure and LLVM's direct call declarations.
inline NativeI64DispatchImage freeze_native_i64_dispatch_image(const Program& program) {
    auto selected = select_native_i64_specializations(program);
    std::unordered_map<std::string, std::size_t> family_sizes;
    for (const auto& fn : program.functions) ++family_sizes[fn.name];
    NativeI64DispatchImage image{
        check_native_i64_specialization_closure(program, selected), {}
    };
    for (const auto& fn : program.functions) {
        const auto selector = native_function_slot(fn.name, fn.parameters.size());
        const auto selection = selected.find(selector);
        if (selection == selected.end()) continue;
        std::vector<std::string> types(fn.generic_parameters.size(), "int");
        std::vector<NativeI64ParameterAbi> parameter_abi;
        parameter_abi.reserve(fn.parameters.size());
        for (const auto& param : fn.parameters)
            parameter_abi.push_back({
                param.name, param.scalar_type, param.mode, param.generic_type,
                param.class_name, param.sum_type
            });
        // Generic native entrypoints are not admitted. Freeze the same
        // module/overload/concrete suffix that codegen must emit.
        const std::string qualified = program.module.empty()
            ? fn.name : program.module + "." + fn.name;
        const std::string emitted_full_symbol =
            (family_sizes.at(fn.name) > 1
                ? native_function_slot(qualified, fn.parameters.size())
                : qualified) + selection->second;
        NativeI64DirectTarget target{
            selector, selection->second, emitted_full_symbol,
            std::move(types), fn.parameters.size(),
            std::move(parameter_abi), fn.return_type, fn.returns_void,
            fn.return_class, fn.return_sum, fn.has_declared_effects,
            fn.declared_effects, fn.exported, fn.native_internal
        };
        verify_native_i64_direct_target(fn, target);
        if (!image.direct_targets.emplace(selector, std::move(target)).second)
            throw std::runtime_error("duplicate frozen native AOT dispatch target");
    }
    if (image.direct_targets.size() != selected.size() ||
        image.closure.concrete_symbols != selected.size())
        throw std::runtime_error("incomplete frozen native AOT direct dispatch image");
    return image;
}

// Predict exact LLVM global symbols *before* declaration, runtime import or
// wrapper emission. The native linker and the per-module IR generator use the
// same namespace. Bare host/runtime symbols and LLVM intrinsic names cannot
// be redefined as ordinary guest functions, regardless of parser provenance.
inline void validate_native_aot_global_symbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() > 1024 ||
        symbol.find('\0') != std::string::npos)
        throw std::runtime_error("invalid native AOT emitted symbol identity");
    if (symbol.rfind("llvm.", 0) == 0)
        throw std::runtime_error("reserved LLVM intrinsic symbol collision");
    // These are exact emitted GLOBAL symbols, not arbitrary source prefixes:
    // a qualified app.write remains distinct from the host write() import.
    static const std::unordered_set<std::string> reserved = {
        "abort", "write", "main", "__oreslang_guest_main",
        "oreslang_cortexm_bootstrap", "ores_guest_entry",
        "ores_browser_abi_version", "ores_browser_open",
        "ores_browser_dispatch", "ores_browser_snapshot",
        "ores_browser_close",
        "ores_string_panic_flush", "ores_string_literal",
        "ores_string_from_i64", "ores_string_from_bool",
        "ores_string_print", "ores_string_concat", "ores_string_equal",
        "ores_string_region_begin", "ores_string_region_end",
        "ores_record_new", "ores_record_get", "ores_record_check_shape",
        "ores_record_check", "ores_record_region_begin",
        "ores_record_region_end", "oreslang_source_actor_send",
        "oreslang_source_actor_spawn", "oreslang_source_actor_stop"
    };
    if (reserved.find(symbol) != reserved.end())
        throw std::runtime_error("reserved native AOT runtime symbol collision");
}

inline std::string plan_native_aot_function_symbol(
        const Program& program, const Function& fn,
        const NativeI64DispatchImage& image, std::size_t family_size,
        bool native_main, bool cortex_m_target) {
    if (family_size == 0)
        throw std::runtime_error("native AOT missing compiler function family");
    const bool cortex_entry = cortex_m_target && fn.name == "main";
    const bool native_entry = native_main && program.module.empty() && fn.name == "main";
    const std::string base = cortex_entry ? "oreslang_cortexm_bootstrap"
        : native_entry ? "__oreslang_guest_main"
        : program.module.empty() ? fn.name : program.module + "." + fn.name;
    const auto* target = image.find_direct(native_function_slot(fn.name, fn.parameters.size()));
    if (target) verify_native_i64_direct_target(fn, *target);
    const std::size_t source_arity =
        native_source_value_arity(fn.parameters.size(), !fn.method_owner.empty());
    const std::string symbol = (family_size > 1
        ? native_function_slot(base, source_arity) : base) +
        (target ? target->emitted_suffix : std::string{});
    if (symbol.empty() || symbol.size() > 1024 ||
        symbol.find('\0') != std::string::npos)
        throw std::runtime_error("invalid native AOT emitted symbol identity");
    if (target && target->emitted_full_symbol != symbol)
        throw std::runtime_error("native AOT frozen direct dispatch symbol mismatch");
    // Only an actual compiler-rewritten native/cortex entry can use this
    // internal implementation symbol. Everything else is checked against the
    // runtime/global namespace before creating any LLVM Function.
    const bool wasm_entry = fn.name == "main" && program.module.empty() &&
        !native_main && !cortex_m_target && family_size == 1;
    if ((!cortex_entry && !native_entry && !wasm_entry) || family_size != 1)
        validate_native_aot_global_symbol(symbol);
    return symbol;
}

// Compiler-to-codegen static call witness for the already-admitted scalar
// specialization slice. The actual LLVM callee must be the exact Function*
// retained from the frozen declaration pass; never rediscover it by name.
// This shape is not a general trait/typeclass witness (no trait solving).
inline void verify_native_i64_scalar_call_witness(
        const NativeI64DirectTarget& target,
        const std::string& resolved_selector,
        const std::string& frozen_symbol,
        const std::string& actual_symbol,
        std::size_t source_argument_count,
        const std::vector<std::string>& emitted_formal_labels,
        bool internal_linkage) {
    if (target.selector != resolved_selector ||
        target.emitted_full_symbol != frozen_symbol ||
        target.lowered_arity != source_argument_count ||
        target.parameter_abi.size() != source_argument_count ||
        !internal_linkage || frozen_symbol != actual_symbol ||
        frozen_symbol.size() <= target.emitted_suffix.size() ||
        frozen_symbol.compare(frozen_symbol.size() - target.emitted_suffix.size(),
                              target.emitted_suffix.size(), target.emitted_suffix) != 0 ||
        emitted_formal_labels.size() != target.parameter_abi.size())
        throw std::runtime_error("native AOT static call witness identity mismatch");
    for (std::size_t i = 0; i < target.parameter_abi.size(); ++i) {
        if (target.parameter_abi[i].name != emitted_formal_labels[i])
            throw std::runtime_error("native AOT static call witness formal ABI mismatch");
    }
}

// Compatibility entry point for the existing source-level C++ verifier.
// Production LLVM lowering consumes NativeI64DispatchImage directly.
inline std::unordered_map<std::string, std::string>
freeze_native_i64_specializations(const Program& program) {
    const auto image = freeze_native_i64_dispatch_image(program);
    std::unordered_map<std::string, std::string> suffixes;
    for (const auto& [selector, target] : image.direct_targets)
        suffixes.emplace(selector, target.emitted_suffix);
    return suffixes;
}

} // namespace oreslang
