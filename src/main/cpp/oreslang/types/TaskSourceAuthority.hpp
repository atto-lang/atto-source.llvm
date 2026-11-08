#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/TaskCaptureAdmission.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang::types {

// Compiler-only gate for the narrow, currently executable capture-free
// Task<int> source subset. It is not general borrowed Task lowering.
inline bool source_task_i64_pure(
        const Expression& expr, std::size_t depth = 0,
        std::size_t* count = nullptr) {
    std::size_t local_count = 0;
    if (!count) count = &local_count;
    if (depth >= 128 || ++*count > 4096) return false;

    using K = Expression::Kind;
    const bool no_side_operands = !expr.condition && !expr.condition_lhs &&
        !expr.condition_rhs && !expr.when_true && !expr.when_false &&
        expr.arguments.empty() && expr.argument_names.empty() &&
        expr.type_arguments.empty();
    if (expr.kind == K::Integer || expr.kind == K::Boolean)
        return no_side_operands && !expr.left && !expr.right;
    if (expr.kind == K::Negate || expr.kind == K::LogicalNot)
        return no_side_operands && expr.left && !expr.right &&
            source_task_i64_pure(*expr.left, depth + 1, count);
    const bool binary =
        expr.kind == K::Add || expr.kind == K::Subtract ||
        expr.kind == K::Multiply || expr.kind == K::ShiftLeft ||
        expr.kind == K::ShiftRight || expr.kind == K::ShiftUnsignedRight ||
        expr.kind == K::Equal || expr.kind == K::NotEqual ||
        expr.kind == K::Less || expr.kind == K::LessEqual ||
        expr.kind == K::Greater || expr.kind == K::GreaterEqual ||
        expr.kind == K::LogicalAnd || expr.kind == K::LogicalOr ||
        expr.kind == K::LogicalXor;
    if (binary)
        return no_side_operands && expr.left && expr.right &&
            source_task_i64_pure(*expr.left, depth + 1, count) &&
            source_task_i64_pure(*expr.right, depth + 1, count);
    if (expr.kind == K::Conditional)
        return !expr.left && !expr.right && expr.arguments.empty() &&
            expr.argument_names.empty() && expr.type_arguments.empty() &&
            expr.when_true && expr.when_false &&
            (expr.condition || (expr.condition_lhs && expr.condition_rhs)) &&
            !(expr.condition && (expr.condition_lhs || expr.condition_rhs)) &&
            (!expr.condition || source_task_i64_pure(*expr.condition, depth + 1, count)) &&
            (!expr.condition_lhs || source_task_i64_pure(*expr.condition_lhs, depth + 1, count)) &&
            (!expr.condition_rhs || source_task_i64_pure(*expr.condition_rhs, depth + 1, count)) &&
            source_task_i64_pure(*expr.when_true, depth + 1, count) &&
            source_task_i64_pure(*expr.when_false, depth + 1, count);
    // Variable, borrow, call, move, share, field, dynamic future and any
    // unknown node fail before LLVM IR rather than inheriting the caller.
    return false;
}

// Compact same-compilation stale-AST detector, not a cryptographic token.
// Every executable child must pass source_task_i64_pure first.
inline std::uint64_t source_task_i64_fingerprint(
        const SourceTaskI64Binding& task) {
    if (task.name.empty() || !task.awaited || !task.result ||
            !source_task_i64_pure(*task.result))
        return 0;

    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto byte = [&](std::uint8_t b) {
        hash ^= b;
        hash *= UINT64_C(1099511628211);
    };
    const auto u64 = [&](std::uint64_t n) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(static_cast<std::uint8_t>(n >> shift));
    };
    const auto str = [&](std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        for (unsigned char c : value) byte(c);
    };
    str(task.name);
    u64(static_cast<std::uint64_t>(task.offset));
    byte(task.awaited ? 1u : 0u);
    const auto expr_digest = [&](const auto& self, const Expression& expr) -> void {
        byte(static_cast<std::uint8_t>(expr.kind));
        u64(static_cast<std::uint64_t>(expr.offset));
        u64(static_cast<std::uint64_t>(expr.integer));
        byte(expr.boolean ? 1u : 0u);
        // Source typing and generic metadata affect native lowering even
        // when the raw expression tree and scalar literal are unchanged.
        // A post-check retype must invalidate the exact ownership proof.
        byte(static_cast<std::uint8_t>(expr.resolved_type));
        str(expr.resolved_class);
        str(expr.sum_type);
        byte(expr.type_arguments_present ? 1u : 0u);
        str(expr.name);
        const auto child = [&](const std::unique_ptr<Expression>& value) {
            byte(value ? 1u : 0u);
            if (value) self(self, *value);
        };
        child(expr.left);
        child(expr.right);
        child(expr.condition);
        child(expr.condition_lhs);
        child(expr.condition_rhs);
        child(expr.when_true);
        child(expr.when_false);
    };
    expr_digest(expr_digest, *task.result);
    return hash == 0 ? UINT64_C(1) : hash;
}

inline std::uint64_t admit_source_task_i64(
        const SourceTaskI64Binding& task, std::uint64_t ownership_epoch) {
    const auto fingerprint = source_task_i64_fingerprint(task);
    if (!fingerprint)
        throw std::runtime_error(
            "ownership violation: source Task<int> lacks capture-free checked result");
    // This assertion is rederived at the semantic pass. The v1 source slice
    // has zero captures; future capture support must supply a real typed list.
    const auto admission = TaskCaptureAdmission::admit(
        &task, ownership_epoch, TaskExecutionDomain::Cpu, {});
    admission.require(&task, ownership_epoch, TaskExecutionDomain::Cpu);
    return fingerprint;
}

// Compiler publication is invalidated on every new check and on any
// downstream semantic failure. Runtime codegen must not consume an earlier
// successful proof after a failed revalidation.
inline void revoke_source_task_i64(SourceTaskI64Binding& task) noexcept {
    task.ownership_admitted = false;
    task.ownership_authority_fingerprint = 0;
    task.ownership_boundary_identity = nullptr;
}

inline void require_source_task_i64(
        const SourceTaskI64Binding& task) {
    const auto current = source_task_i64_fingerprint(task);
    if (!task.ownership_admitted ||
            task.ownership_boundary_identity != &task ||
            !task.ownership_authority_fingerprint || !current ||
            task.ownership_authority_fingerprint != current)
        throw std::runtime_error(
            "source Task<int> lacks exact post-ownership lowering authority");
}

} // namespace oreslang::types
