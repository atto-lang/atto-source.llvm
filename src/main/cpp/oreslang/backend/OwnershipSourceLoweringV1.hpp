#pragma once

#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace oreslang {

// Translate a resolver/typechecked source Function into the bounded ownership
// CFG. This first source adapter is intentionally narrow: it covers the same
// production-checked two-return actor-region guest subset and rejects every
// source construct not explicitly represented below. It is not a substitute
// for the general compiler CFG; that integration must map all Statement,
// unwind, suspension, cancellation and scheduler edges before admission.
class OwnershipSourceLoweringV1 final {
public:
    static OwnershipV1Result lower_guarded_owned_class_function(
        const Program& program, const Function& fn, const ClassDecl& cls) {
        reject_if(!program.imports.empty() || !program.actors.empty() ||
                  !program.generators.empty() || !program.cooperative_loops.empty() ||
                  !program.sum_types.empty() || !program.type_only_declarations.empty() ||
                  !program.type_constraint_descriptors.empty() ||
                  !program.contracts.empty() || !program.conforms.empty() ||
                  program.classes.size() != 1 || program.functions.size() != 1 ||
                  &program.functions.front() != &fn || &program.classes.front() != &cls,
                  "program roots are not closed or AST references are not canonical");
        reject_if(fn.name != "main" || !fn.parameters.empty() ||
                  fn.locals.empty() || fn.locals.size() > 8 ||
                  fn.statements.size() != 0 || fn.early_returns.size() != 1 ||
                  !fn.result || fn.returns_void || fn.return_type != ScalarType::I64 ||
                  !fn.return_class.empty() || !fn.return_sum.empty() ||
                  fn.scalar_while || fn.async_source_i64 ||
                  !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
                  !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                  !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
                  !fn.actor_field_writes.empty() || fn.caught_throw_value ||
                  !fn.caught_throw_defers.empty() || !fn.ready_await_actor.empty() ||
                  !fn.done_await_actor.empty() || !fn.ready_future_name.empty() ||
                  !fn.ready_future_actor.empty() || fn.has_declared_effects ||
                  fn.native_internal || !fn.method_owner.empty() ||
                  !fn.c_export_u32_symbol.empty() || !fn.generic_parameters.empty(),
                  "function contains an unmodeled source/CFG feature");
        reject_if(cls.fields.empty() || cls.fields.size() > 16 ||
                  !cls.methods.empty() || cls.explicit_constructor ||
                  !cls.constructor_parameters.empty(),
                  "class layout or dispatch is not admitted");

        OwnershipV1Graph graph;
        std::unordered_map<std::string, std::size_t> owner_ids;
        for (const auto& local : fn.locals) {
            reject_if(!local.new_class || local.class_name != cls.name ||
                      local.name.empty() || local.record_view || local.move_class ||
                      local.heap_owned || local.int_array || local.owner_mutable ||
                      local.field_mutable || local.assignable || local.initializer ||
                      !local.constructor_arguments.empty() ||
                      !local.move_source.empty() || !local.sum_type.empty() ||
                      !local.pending_type_alias.empty() ||
                      !local.task_await_name.empty() || !local.future_await_name.empty(),
                      "local is not a fresh, nonescaping class owner");
            const auto id = graph.owners.size();
            if (!owner_ids.emplace(local.name, id).second)
                reject("duplicate owner binding");
            graph.owners.push_back(local.name);
        }

        for (const auto& field : cls.fields) {
            const auto* initial = field.initializer_expression.get();
            reject_if(!field.public_member || !field.final_field ||
                      field.mutable_referent || field.scalar_type != ScalarType::I64 ||
                      field.int_array || !field.class_name.empty() ||
                      field.constructor_argument || !field.sum_type.empty() ||
                      !initial || initial->kind != Expression::Kind::Integer ||
                      initial->left || initial->right || initial->condition ||
                      initial->condition_lhs || initial->condition_rhs ||
                      initial->when_true || initial->when_false ||
                      !initial->arguments.empty() || !initial->argument_names.empty() ||
                      !initial->type_arguments.empty() || initial->type_arguments_present || !initial->sum_type.empty(),
                      "class field is not an immutable literal scalar");
        }

        const auto field_owner = [&](const Expression* expression) -> std::size_t {
            reject_if(!expression || expression->kind != Expression::Kind::Field ||
                      !expression->left ||
                      expression->left->kind != Expression::Kind::Variable ||
                      expression->left->left || expression->left->right ||
                      expression->left->condition || expression->left->when_true ||
                      expression->left->when_false || expression->left->condition_lhs ||
                      expression->left->condition_rhs ||
                      !expression->left->argument_names.empty() ||
                      !expression->left->type_arguments.empty() ||
                      expression->left->type_arguments_present ||
                      !expression->left->sum_type.empty() ||
                      !expression->left->arguments.empty() || !expression->left->argument_names.empty() || expression->left->type_arguments_present || !expression->left->sum_type.empty() ||
                      !expression->left->type_arguments.empty() ||
                      expression->right || expression->condition ||
                      expression->condition_lhs || expression->condition_rhs ||
                      expression->when_true || expression->when_false ||
                      !expression->arguments.empty() ||
                      !expression->argument_names.empty() ||
                      !expression->type_arguments.empty() || expression->type_arguments_present || !expression->sum_type.empty(),
                      "field access is not a direct owner.field expression");
            const auto found = owner_ids.find(expression->left->name);
            if (found == owner_ids.end()) reject("field access uses a non-owned receiver");
            bool declared = false;
            for (const auto& field : cls.fields)
                if (field.name == expression->name) declared = true;
            if (!declared) reject("field access names an unknown field");
            return found->second;
        };

        const auto& guard = fn.early_returns.front();
        reject_if(!guard.condition || guard.condition->kind != Expression::Kind::Equal ||
                  !guard.condition->left || !guard.condition->right ||
                  guard.condition->right->kind != Expression::Kind::Integer ||
                  guard.condition->right->left || guard.condition->right->right ||
                  guard.condition->right->condition ||
                  guard.condition->right->condition_lhs ||
                  guard.condition->right->condition_rhs ||
                  guard.condition->right->when_true ||
                  guard.condition->right->when_false ||
                  !guard.condition->right->arguments.empty() || !guard.condition->right->argument_names.empty() || guard.condition->right->type_arguments_present || !guard.condition->right->sum_type.empty() ||
                  !guard.condition->right->type_arguments.empty() ||
                  guard.condition->condition || guard.condition->condition_lhs ||
                  guard.condition->condition_rhs || guard.condition->when_true ||
                  guard.condition->when_false || !guard.condition->arguments.empty() ||
                  !guard.condition->argument_names.empty() ||
                  !guard.condition->type_arguments.empty() || !guard.condition->sum_type.empty() ||
                  guard.condition->type_arguments_present,
                  "guard is not a pure equality against an integer literal");

        const auto guard_owner = field_owner(guard.condition->left.get());
        const auto early_owner = field_owner(guard.result.get());
        const auto normal_owner = field_owner(fn.result.get());

        // Build directly from source ownership and source expression uses:
        // owners are acquired in lexical order; the guard read occurs before
        // branching; each return expression is read on its own successor.
        // Both source exits are explicit, and all owners must be available at
        // both exits. Branch-local owners are rejected until their own edge
        // cleanup plans are implemented.
        graph.entry = 0;
        graph.blocks.resize(3);
        auto& entry = graph.blocks[0];
        for (std::size_t owner = 0; owner < graph.owners.size(); ++owner)
            entry.operations.push_back({OwnershipV1OpKind::Create, owner});
        entry.operations.push_back({OwnershipV1OpKind::Use, guard_owner});
        entry.successors = {1, 2};
        graph.blocks[1].operations.push_back({OwnershipV1OpKind::Use, early_owner});
        graph.blocks[1].operations.push_back({OwnershipV1OpKind::ExitNormal, 0});
        graph.blocks[2].operations.push_back({OwnershipV1OpKind::Use, normal_owner});
        graph.blocks[2].operations.push_back({OwnershipV1OpKind::ExitNormal, 0});
        return OwnershipDataflowV1::analyze(graph);
    }

private:
    [[noreturn]] static void reject(const char* message) {
        throw std::runtime_error(std::string("OwnershipSourceLoweringV1: ") + message);
    }
    static void reject_if(bool condition, const char* message) {
        if (condition) reject(message);
    }
};

} // namespace oreslang
