#include "oreslang/types/TaskSourceAuthority.hpp"

// Safety assertions must also execute under Release/-DNDEBUG builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

using oreslang::Expression;
using oreslang::SourceTaskI64Binding;
using oreslang::types::admit_source_task_i64;
using oreslang::types::require_source_task_i64;
using oreslang::types::source_task_i64_fingerprint;

static std::unique_ptr<Expression> expr(Expression::Kind kind) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    return result;
}

static bool denied(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

int main() {
    SourceTaskI64Binding task;
    task.name = "worker";
    task.awaited = true;
    task.offset = 7;
    task.result = expr(Expression::Kind::Add);
    task.result->left = expr(Expression::Kind::Integer);
    task.result->left->integer = 41;
    task.result->right = expr(Expression::Kind::Integer);
    task.result->right->integer = 1;

    assert(denied([&] { require_source_task_i64(task); }));
    const auto fingerprint = admit_source_task_i64(task, 37);
    assert(fingerprint && fingerprint == source_task_i64_fingerprint(task));
    task.ownership_authority_fingerprint = fingerprint;
    task.ownership_boundary_identity = &task;
    task.ownership_admitted = true;
    require_source_task_i64(task);

    // Copying both the proof bit and its content fingerprint cannot give
    // authority to a different AST allocation. Only the originally checked
    // Task boundary may enter codegen; clones need their own proof pass.
    SourceTaskI64Binding clone;
    clone.name = task.name;
    clone.offset = task.offset;
    clone.awaited = task.awaited;
    clone.result = expr(Expression::Kind::Add);
    clone.result->left = expr(Expression::Kind::Integer);
    clone.result->left->integer = 41;
    clone.result->right = expr(Expression::Kind::Integer);
    clone.result->right->integer = 1;
    clone.ownership_authority_fingerprint = fingerprint;
    clone.ownership_boundary_identity = &task;
    clone.ownership_admitted = true;
    assert(source_task_i64_fingerprint(clone) == fingerprint);
    assert(denied([&] { require_source_task_i64(clone); }));
    task.ownership_boundary_identity = nullptr;
    assert(denied([&] { require_source_task_i64(task); }));
    task.ownership_boundary_identity = &task;
    require_source_task_i64(task);

    // Modifying an already checked Task must invalidate its IR permission.
    task.result->left->integer = 40;
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->left->integer = 41;
    require_source_task_i64(task);
    task.offset++;
    assert(denied([&] { require_source_task_i64(task); }));
    task.offset--;
    require_source_task_i64(task);
    // The same source expression with different type-checker metadata must
    // never inherit an earlier native ownership admission.
    task.result->resolved_type = oreslang::ScalarType::Bool;
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->resolved_type = oreslang::ScalarType::I64;
    require_source_task_i64(task);
    task.result->resolved_class = "unverified";
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->resolved_class.clear();
    require_source_task_i64(task);
    task.result->sum_type = "Result<int,int>";
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->sum_type.clear();
    require_source_task_i64(task);
    task.result->type_arguments_present = true;
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->type_arguments_present = false;
    require_source_task_i64(task);
    task.result->right = expr(Expression::Kind::Variable);
    task.result->right->name = "parent_owned";
    assert(!source_task_i64_fingerprint(task));
    assert(denied([&] { require_source_task_i64(task); }));
    task.result->right = expr(Expression::Kind::Integer);
    task.result->right->integer = 1;

    // Parser-only claims cannot forge a complete ownership pass.
    task.ownership_admitted = false;
    assert(denied([&] { require_source_task_i64(task); }));
    task.ownership_admitted = true;
    task.ownership_authority_fingerprint = 0;
    assert(denied([&] { require_source_task_i64(task); }));
    task.ownership_authority_fingerprint = fingerprint;
    require_source_task_i64(task);

    // Nonexistent runtime outcome, hidden expression children and malformed
    // ASTs must all fail closed even if a stale admission bit survived.
    task.awaited = false;
    assert(denied([&] { admit_source_task_i64(task, 38); }));
    task.awaited = true;
    task.result->arguments.push_back(expr(Expression::Kind::Integer));
    assert(denied([&] { admit_source_task_i64(task, 38); }));
    task.result->arguments.clear();
    task.result->kind = Expression::Kind::Borrow;
    assert(denied([&] { admit_source_task_i64(task, 38); }));
    task.result->kind = Expression::Kind::Add;
    assert(denied([&] { admit_source_task_i64(task, 0); }));
    assert(admit_source_task_i64(task, 39) == fingerprint);
    return 0;
}
