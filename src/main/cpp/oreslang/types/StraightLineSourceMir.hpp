#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipCfg.hpp"
#include "oreslang/types/SourceNodeIdentity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::native_mir {

// A *complete trace only for an explicitly whitelisted source sublanguage*:
// literal class construction, direct scalar/local field reads and moves,
// sequential direct field/scalar writes, and an eager scalar return.
// Unsupported calls, branches, cleanup, loans, arrays and suspensions return
// nullopt rather than publishing a false whole-function ownership proof.
// The graph is produced from the independently ownership-checked AST, not
// reverse-engineered from emitted LLVM instructions or test-authored claims.
enum class SourcePhase { Local, Statement, Result };
struct SourceStep {
    ownership::Operation operation;
    SourcePhase phase = SourcePhase::Local;
    std::size_t ordinal = 0;
    std::uint64_t statement_node_id = 0;
    std::size_t source_offset = 0;
};
struct StraightLineTrace {
    ownership::Graph graph;
    std::vector<SourceStep> steps;
    std::uint64_t compilation_unit_id = 0;
    std::uint64_t source_function_id = 0;
    bool supported_source_effects_complete = false;
    // Never imply a complete native SSA/LoanId/optimized ownership proof.
    bool full_loan_use_coverage = false;
    bool optimized_code_verified = false;
    bool guest_verified = false;
};

inline std::optional<StraightLineTrace> build_checked_straight_line_trace(
        const Program& program, const Function& function) {
    source_identity::validate_program_identity(program);
    const bool exact_function = std::any_of(
        program.functions.begin(), program.functions.end(),
        [&](const Function& item) { return &item == &function; });
    if (!exact_function || !function.compiler_source_function_id)
        throw std::runtime_error(
            "ownership violation: MIR source function is not from sealed Program");

    if (function.return_type == ScalarType::String ||
            function.compiler_straight_line_effect_fingerprint == 0)
        return std::nullopt;
    if (!function.parameters.empty() || !function.early_returns.empty() ||
            !function.actor_spawns.empty() || !function.actor_actions.empty() ||
            !function.actor_sends.empty() || !function.actor_stops.empty() ||
            !function.actor_field_writes.empty() || !function.source_tasks.empty() ||
            !function.future_pipelines.empty() || function.async_source_i64 ||
            function.scalar_while || function.caught_throw_value ||
            !function.caught_throw_defers.empty() ||
            !function.ready_await_actor.empty() ||
            !function.done_await_actor.empty() ||
            !function.method_owner.empty() || !function.return_class.empty() ||
            function.returns_void || !function.result ||
            function.locals.size() > 64 || function.statements.size() > 128)
        return std::nullopt;

    StraightLineTrace trace;
    trace.compilation_unit_id = program.compiler_source_unit_id;
    trace.source_function_id = function.compiler_source_function_id;
    trace.graph.blocks.resize(1);
    std::map<std::string, std::set<std::string>> class_fields;
    std::set<std::string> names;
    std::set<std::string> mutable_class_owners;
    std::set<std::string> assignable_scalar_slots;
    std::size_t modeled_fields = 0;
    for (const auto& local : function.locals) {
        if (local.name.empty() || !names.insert(local.name).second ||
                local.scalar_type == ScalarType::String ||
                local.heap_owned || local.int_array || local.record_view ||
                local.move_class || !local.task_await_name.empty() ||
                !local.future_await_name.empty() ||
                !local.constructor_arguments.empty())
            return std::nullopt;
        if (!local.class_name.empty()) {
            if (!local.new_class || local.initializer)
                return std::nullopt;
            if (local.owner_mutable)
                mutable_class_owners.insert(local.name);
            const auto cls = std::find_if(program.classes.begin(),
                program.classes.end(), [&](const ClassDecl& c) {
                    return c.name == local.class_name;
                });
            if (cls == program.classes.end() || cls->fields.empty() ||
                    cls->fields.size() > 16)
                return std::nullopt;
            auto& fields = class_fields[local.name];
            for (const auto& field : cls->fields) {
                if (field.name.empty() || field.name.find('.') != std::string::npos ||
                        field.int_array || !field.class_name.empty() ||
                        field.scalar_type == ScalarType::String ||
                        field.final_field || field.constructor_argument ||
                        !field.initializer_expression ||
                        (field.initializer_expression->kind != Expression::Kind::Integer &&
                         field.initializer_expression->kind != Expression::Kind::Boolean) ||
                        !fields.insert(field.name).second)
                    return std::nullopt;
                if (++modeled_fields > 128)
                    return std::nullopt;
                trace.graph.entry.emplace(
                    local.name + "." + field.name,
                    ownership::PlaceState(ownership::kUninitialized));
            }
        } else {
            if (local.new_class || !local.initializer)
                return std::nullopt;
            if (local.assignable)
                assignable_scalar_slots.insert(local.name);
            trace.graph.entry.emplace(
                local.name, ownership::PlaceState(ownership::kUninitialized));
        }
    }
    if (class_fields.empty())
        return std::nullopt;

    const auto record = [&](ownership::Operation::Kind kind,
            const std::string& place, SourcePhase phase, std::size_t ordinal,
            std::uint64_t node, std::size_t offset,
            std::optional<std::uint64_t> write = std::nullopt) -> bool {
        if (trace.steps.size() >= 512 || !trace.graph.entry.count(place))
            return false;
        ownership::Operation op(kind, place, std::nullopt, write);
        trace.graph.blocks[0].operations.push_back(op);
        trace.steps.push_back({op, phase, ordinal, node, offset});
        return true;
    };
    const auto expression = [&](const auto& self, const Expression& expr,
            SourcePhase phase, std::size_t ordinal,
            std::uint64_t node, std::size_t depth) -> bool {
        if (depth > 32)
            return false;
        switch (expr.kind) {
            case Expression::Kind::Integer:
            case Expression::Kind::Boolean:
                return true;
            case Expression::Kind::Variable:
                // Reading a whole class requires all of its fields, but is
                // not in this first simple scalar-place trace.
                return !class_fields.count(expr.name) &&
                    record(ownership::Operation::Kind::Read, expr.name,
                        phase, ordinal, node, expr.offset);
            case Expression::Kind::Field:
                if (!expr.left ||
                        expr.left->kind != Expression::Kind::Variable ||
                        !class_fields.count(expr.left->name) ||
                        !class_fields.at(expr.left->name).count(expr.name))
                    return false;
                return record(ownership::Operation::Kind::Read,
                    expr.left->name + "." + expr.name,
                    phase, ordinal, node, expr.offset);
            case Expression::Kind::Take:
                if (!expr.left)
                    return false;
                if (expr.left->kind == Expression::Kind::Variable &&
                        !class_fields.count(expr.left->name))
                    return record(ownership::Operation::Kind::Move,
                        expr.left->name, phase, ordinal, node, expr.offset);
                if (expr.left->kind == Expression::Kind::Field &&
                        expr.left->left &&
                        expr.left->left->kind == Expression::Kind::Variable &&
                        class_fields.count(expr.left->left->name) &&
                        class_fields.at(expr.left->left->name).count(expr.left->name))
                    return record(ownership::Operation::Kind::Move,
                        expr.left->left->name + "." + expr.left->name,
                        phase, ordinal, node, expr.offset);
                return false;
            case Expression::Kind::Negate:
            case Expression::Kind::LogicalNot:
                return expr.left && self(self, *expr.left,
                    phase, ordinal, node, depth + 1);
            // Eager scalar operators only. Lazy and/or/conditional, calls,
            // borrowing, sharing, destructuring, arrays and sum operations
            // explicitly require future full control-flow/effect lowering.
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
            case Expression::Kind::Equal:
            case Expression::Kind::NotEqual:
            case Expression::Kind::Less:
            case Expression::Kind::LessEqual:
            case Expression::Kind::Greater:
            case Expression::Kind::GreaterEqual:
                return expr.left && expr.right &&
                    self(self, *expr.left, phase, ordinal, node, depth + 1) &&
                    self(self, *expr.right, phase, ordinal, node, depth + 1);
            default:
                return false;
        }
    };

    // Define all source owner slots once, then replay actual local
    // initialization order before any user statements or return reads.
    for (std::size_t i = 0; i < function.locals.size(); ++i) {
        const auto& local = function.locals[i];
        if (class_fields.count(local.name)) {
            for (const auto& field : class_fields.at(local.name))
                if (!record(ownership::Operation::Kind::Initialize,
                        local.name + "." + field,
                        SourcePhase::Local, i, 0, local.offset))
                    return std::nullopt;
        } else {
            if (!expression(expression, *local.initializer,
                    SourcePhase::Local, i, 0, 0) ||
                    !record(ownership::Operation::Kind::Initialize,
                        local.name, SourcePhase::Local, i, 0, local.offset))
                return std::nullopt;
        }
    }

    std::map<std::size_t, const CheckedSourceReinitializeOperation*> checked;
    for (const auto& op : function.compiler_checked_reinitialize_operations)
        if (!checked.emplace(op.statement_index, &op).second)
            throw std::runtime_error(
                "ownership violation: duplicate compiler MIR reinit ordinal");

    for (std::size_t i = 0; i < function.statements.size(); ++i) {
        const auto& stmt = function.statements[i];
        if (!stmt.expression ||
                (stmt.kind != Statement::Kind::FieldWrite &&
                 stmt.kind != Statement::Kind::ScalarWrite) ||
                !stmt.then_statements.empty() || !stmt.else_statements.empty())
            return std::nullopt;
        const auto node = stmt.compiler_source_node_id;
        if (!expression(expression, *stmt.expression,
                SourcePhase::Statement, i, node, 0))
            return std::nullopt;
        if (stmt.kind == Statement::Kind::ScalarWrite) {
            if (!assignable_scalar_slots.count(stmt.receiver))
                return std::nullopt;
            if (checked.count(i) ||
                    !record(ownership::Operation::Kind::WriteOwned,
                        stmt.receiver, SourcePhase::Statement, i, node, stmt.offset))
                return std::nullopt;
            continue;
        }
        if (!class_fields.count(stmt.receiver) ||
                !mutable_class_owners.count(stmt.receiver) ||
                !class_fields.at(stmt.receiver).count(stmt.field))
            return std::nullopt;
        const std::string place = stmt.receiver + "." + stmt.field;
        const auto found = checked.find(i);
        if (found != checked.end()) {
            const auto& proof = *found->second;
            if (!stmt.compiler_checked_field_reinitialization ||
                    !stmt.compiler_reinitialize_write_id ||
                    proof.sealed_write_id != stmt.compiler_reinitialize_write_id ||
                    proof.canonical_place != place ||
                    proof.source_offset != stmt.offset ||
                    !record(ownership::Operation::Kind::Reinitialize, place,
                        SourcePhase::Statement, i, node, stmt.offset,
                        proof.sealed_write_id))
                throw std::runtime_error(
                    "ownership violation: checker MIR reinitialization receipt mismatch");
        } else {
            if (stmt.compiler_checked_field_reinitialization)
                throw std::runtime_error(
                    "ownership violation: source reinit missing from compiler MIR");
            if (!record(ownership::Operation::Kind::WriteOwned, place,
                    SourcePhase::Statement, i, node, stmt.offset))
                return std::nullopt;
        }
    }
    if (!expression(expression, *function.result,
            SourcePhase::Result, 0, 0, 0))
        return std::nullopt;

    if (trace.steps.size() != trace.graph.blocks[0].operations.size())
        throw std::runtime_error(
            "ownership violation: straight-line MIR source operation inventory mismatch");
    // All admitted operations in this narrow, fully enumerated source
    // sublanguage are replayed by the existing field-sensitive CFG lattice.
    // Any failed transition means the source/evidence is not sound.
    const auto checked_graph = ownership::verify(trace.graph);
    if (checked_graph.outgoing.size() != 1 ||
            !checked_graph.outgoing[0].has_value())
        throw std::runtime_error(
            "ownership violation: straight-line MIR graph has no verified exit");
    trace.supported_source_effects_complete = true;
    return trace;
}

} // namespace oreslang::native_mir
