#include "oreslang/frontend/MethodResolver.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace oreslang {
void resolve_native_methods(Program& program) {
    std::unordered_map<std::string, const ClassDecl*> classes;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    for (auto& fn : program.functions) {
        if (!fn.method_owner.empty()) {
            if (!classes.count(fn.method_owner) || fn.parameters.empty()
                    || fn.parameters.front().name != "self"
                    || fn.parameters.front().class_name != fn.method_owner)
                throw std::runtime_error("invalid compiler-owned method receiver");
            fn.native_internal = true; // source visibility is not a raw FFI export
        }
        std::unordered_map<std::string, std::string> owners;
        for (const auto& parameter : fn.parameters)
            if (!parameter.class_name.empty()) owners.emplace(parameter.name, parameter.class_name);
        for (const auto& binding : fn.locals)
            if (!binding.class_name.empty()) owners.emplace(binding.name, binding.class_name);
        std::function<void(Expression*)> expression = [&](Expression* expr) {
            if (!expr) return;
            expression(expr->left.get()); expression(expr->right.get());
            expression(expr->condition.get()); expression(expr->condition_lhs.get()); expression(expr->condition_rhs.get());
            expression(expr->when_true.get()); expression(expr->when_false.get());
            for (auto& arg : expr->arguments) expression(arg.get());
            if (expr->kind != Expression::Kind::ArrayCall || !expr->left
                    || expr->left->kind != Expression::Kind::Variable) return;
            auto owner = owners.find(expr->left->name);
            if (owner == owners.end()) return; // existing Array<int> path validates separately
            auto cls = classes.find(owner->second);
            if (cls == classes.end()) throw std::runtime_error("unknown instance method class");
            const ClassMethod* method = nullptr;
            for (const auto& candidate : cls->second->methods)
                if (candidate.name == expr->name && candidate.arity == expr->arguments.size()) method = &candidate;
            if (!method) throw std::runtime_error("unknown method or wrong explicit call arity '" + expr->name + "'");
            if (!method->public_member && fn.method_owner != cls->first)
                throw std::runtime_error("private instance method is not accessible");
            auto loan = std::make_unique<Expression>();
            loan->kind = method->mutable_receiver ? Expression::Kind::BorrowMut : Expression::Kind::Borrow;
            loan->offset = expr->offset; loan->left = std::move(expr->left);
            expr->kind = Expression::Kind::Call;
            expr->name = cls->first + "." + method->name;
            if (!expr->argument_names.empty()) expr->argument_names.insert(expr->argument_names.begin(), "");
            expr->arguments.insert(expr->arguments.begin(), std::move(loan));
        };
        std::function<void(Statement&)> statement = [&](Statement& stmt) {
            expression(stmt.expression.get()); expression(stmt.condition.get()); expression(stmt.condition_lhs.get()); expression(stmt.condition_rhs.get());
            expression(stmt.array_commit_length.get());
            for (auto& item : stmt.array_commit_values) expression(item.get());
            if (stmt.branch_local) expression(stmt.branch_local->initializer.get());
            for (auto& arm : stmt.then_statements) statement(arm);
            for (auto& arm : stmt.else_statements) statement(arm);
        };
        for (auto& local : fn.locals) {
            expression(local.initializer.get());
            for (auto& arg : local.constructor_arguments) expression(arg.get());
        }
        for (auto& item : fn.statements) statement(item);
        for (auto& item : fn.caught_throw_defers) statement(item);
        for (auto& guard : fn.early_returns) { expression(guard.condition.get()); expression(guard.lhs.get()); expression(guard.rhs.get()); expression(guard.result.get()); }
        for (auto& send : fn.actor_sends) expression(send.payload.get());
        for (auto& write : fn.actor_field_writes) expression(write.expression.get());
        expression(fn.caught_throw_value.get()); expression(fn.result.get());
    }
}
}
