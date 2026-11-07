#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace oreslang::source_identity {

// Called by the parser/resolver only *after* full validation has completed.
// The minted values are independent of source offsets and allocator addresses.
// A fresh compilation gets a distinct unit identity; IDs within that unit
// represent the exact validated AST nodes and are never recycled.
inline void seal_validated_program(Program& program) {
    if (program.compiler_source_unit_id != 0)
        throw std::runtime_error(
            "ownership violation: validated source identity already sealed");
    constexpr std::size_t kMaxFunctions = 8192;
    constexpr std::uint64_t kMaxNodes = 1048576;
    constexpr std::size_t kMaxDepth = 64;
    if (program.functions.size() > kMaxFunctions)
        throw std::runtime_error(
            "ownership violation: source function identity inventory exceeds bound");

    // Check source-node bounds before publishing a unit identity.
    std::uint64_t count = 0;
    const auto count_tree = [&](const auto& self,
            const Statement& statement, std::size_t depth) -> void {
        if (depth > kMaxDepth || count == kMaxNodes)
            throw std::runtime_error(
                "ownership violation: source node identity traversal exceeds bound");
        if (statement.compiler_source_node_id ||
                !statement.compiler_source_path.empty() ||
                statement.compiler_reinitialize_write_id ||
                (statement.compiler_checked_field_reinitialization &&
                 statement.kind != Statement::Kind::FieldWrite))
            throw std::runtime_error(
                "ownership violation: preassigned source node identity");
        ++count;
        for (const auto& child : statement.then_statements)
            self(self, child, depth + 1);
        for (const auto& child : statement.else_statements)
            self(self, child, depth + 1);
    };
    for (const auto& function : program.functions) {
        if (function.compiler_source_function_id ||
                !function.compiler_source_sealed_name.empty())
            throw std::runtime_error(
                "ownership violation: preassigned source function identity");
        if (function.compiler_checked_reinitialize_operations.size() > 4096)
            throw std::runtime_error(
                "ownership violation: source reinit operation count exceeds bound");
        for (const auto& effect : function.compiler_checked_reinitialize_operations)
            if (effect.sealed_write_id)
                throw std::runtime_error(
                    "ownership violation: preassigned source reinit effect identity");
        for (const auto& stmt : function.statements)
            count_tree(count_tree, stmt, 0);
        for (const auto& stmt : function.caught_throw_defers)
            count_tree(count_tree, stmt, 0);
    }

    // Global process-local epoch, with atomic uniqueness across concurrent
    // compilations. Overflow cannot silently recycle an old unit identity.
    static std::atomic<std::uint64_t> next_unit{1};
    std::uint64_t identity = next_unit.load(std::memory_order_relaxed);
    for (;;) {
        if (identity == 0 || identity == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error(
                "ownership violation: exhausted compiler source unit identities");
        if (next_unit.compare_exchange_weak(identity, identity + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }

    std::uint64_t next_node = 0;
    const auto mint_tree = [&](const auto& self, Statement& stmt,
            const std::string& path) -> void {
        stmt.compiler_source_node_id = ++next_node;
        stmt.compiler_source_path = path;
        // OwnershipChecker marked this actual direct moved-field restore;
        // source identity minting fixes its claim before LLVM codegen. The
        // ordinal belongs to the validated AST, not to emitted LLVM stores.
        stmt.compiler_reinitialize_write_id =
            stmt.compiler_checked_field_reinitialization
                ? stmt.compiler_source_node_id : 0;
        for (std::size_t i = 0; i < stmt.then_statements.size(); ++i)
            self(self, stmt.then_statements[i],
                path + "/t" + std::to_string(i));
        for (std::size_t i = 0; i < stmt.else_statements.size(); ++i)
            self(self, stmt.else_statements[i],
                path + "/e" + std::to_string(i));
    };
    std::uint64_t function_id = 0;
    for (auto& function : program.functions) {
        function.compiler_source_function_id = ++function_id;
        function.compiler_source_sealed_name = function.name;
        const std::string prefix = "f" + std::to_string(function_id);
        for (std::size_t i = 0; i < function.statements.size(); ++i)
            mint_tree(mint_tree, function.statements[i],
                prefix + "/s" + std::to_string(i));
        for (std::size_t i = 0; i < function.caught_throw_defers.size(); ++i)
            mint_tree(mint_tree, function.caught_throw_defers[i],
                prefix + "/c" + std::to_string(i));
        std::set<std::size_t> seen_effects;
        for (auto& effect : function.compiler_checked_reinitialize_operations) {
            if (effect.statement_index >= function.statements.size() ||
                    !seen_effects.insert(effect.statement_index).second)
                throw std::runtime_error(
                    "ownership violation: duplicate or invalid checker operation ordinal");
            const auto& statement = function.statements[effect.statement_index];
            if (!statement.compiler_checked_field_reinitialization ||
                    statement.compiler_reinitialize_write_id == 0 ||
                    statement.receiver + "." + statement.field !=
                        effect.canonical_place ||
                    statement.offset != effect.source_offset)
                throw std::runtime_error(
                    "ownership violation: unchecked or mismatched compiler reinit operation");
            effect.sealed_write_id = statement.compiler_reinitialize_write_id;
        }
        for (std::size_t i = 0; i < function.statements.size(); ++i)
            if (function.statements[i].compiler_checked_field_reinitialization &&
                    !seen_effects.count(i))
                throw std::runtime_error(
                    "ownership violation: reinit marker absent from checker operation inventory");
    }
    // Publish only after all identities have been minted. A truncated tail
    // could otherwise preserve a valid 1..N prefix of node identities.
    program.compiler_source_node_count = next_node;
    program.compiler_source_function_count = function_id;
    program.compiler_source_unit_id = identity;
}

// Read-only safety check. It catches erased/replaced nodes, stale epochs,
// duplicate IDs and copied identities even if malloc reuses the same address.
// No pointer supplied by the caller serves as the proof of node identity.
inline void validate_program_identity(const Program& program) {
    if (!program.compiler_source_unit_id)
        throw std::runtime_error(
            "ownership violation: source compilation identity is unsealed");
    std::set<std::uint64_t> node_ids;
    std::uint64_t expected_node = 0;
    std::uint64_t expected_function = 0;
    constexpr std::uint64_t kMaxNodes = 1048576;
    constexpr std::size_t kMaxDepth = 64;
    const auto visit = [&](const auto& self,
            const Statement& stmt, std::size_t depth,
            const std::string& expected_path) -> void {
        if (depth > kMaxDepth || expected_node == kMaxNodes)
            throw std::runtime_error(
                "ownership violation: source identity traversal exceeds bound");
        ++expected_node;
        if (stmt.compiler_source_node_id != expected_node ||
                stmt.compiler_source_path != expected_path ||
                (stmt.compiler_checked_field_reinitialization &&
                    stmt.kind != Statement::Kind::FieldWrite) ||
                stmt.compiler_reinitialize_write_id !=
                    (stmt.compiler_checked_field_reinitialization
                        ? stmt.compiler_source_node_id : 0) ||
                !node_ids.insert(stmt.compiler_source_node_id).second)
            throw std::runtime_error(
                "ownership violation: stale, copied, reparented or missing source node identity");
        for (std::size_t i = 0; i < stmt.then_statements.size(); ++i)
            self(self, stmt.then_statements[i], depth + 1,
                expected_path + "/t" + std::to_string(i));
        for (std::size_t i = 0; i < stmt.else_statements.size(); ++i)
            self(self, stmt.else_statements[i], depth + 1,
                expected_path + "/e" + std::to_string(i));
    };
    if (program.functions.size() > 8192 ||
            program.functions.size() != program.compiler_source_function_count)
        throw std::runtime_error(
            "ownership violation: source function count or identity mismatch");
    for (const auto& function : program.functions) {
        if (function.compiler_source_function_id != ++expected_function ||
                function.compiler_source_sealed_name != function.name)
            throw std::runtime_error(
                "ownership violation: missing, reordered or renamed source function identity");
        const std::string prefix = "f" + std::to_string(expected_function);
        for (std::size_t i = 0; i < function.statements.size(); ++i)
            visit(visit, function.statements[i], 0,
                prefix + "/s" + std::to_string(i));
        for (std::size_t i = 0; i < function.caught_throw_defers.size(); ++i)
            visit(visit, function.caught_throw_defers[i], 0,
                prefix + "/c" + std::to_string(i));
        if (function.compiler_checked_reinitialize_operations.size() > 4096)
            throw std::runtime_error(
                "ownership violation: oversized checker operation inventory");
        std::set<std::size_t> checked_effects;
        for (const auto& effect : function.compiler_checked_reinitialize_operations) {
            if (effect.statement_index >= function.statements.size() ||
                    !checked_effects.insert(effect.statement_index).second)
                throw std::runtime_error(
                    "ownership violation: duplicate or out-of-range checker reinit operation");
            const auto& statement = function.statements[effect.statement_index];
            if (!statement.compiler_checked_field_reinitialization ||
                    !statement.compiler_reinitialize_write_id ||
                    statement.kind != Statement::Kind::FieldWrite ||
                    effect.sealed_write_id !=
                        statement.compiler_reinitialize_write_id ||
                    effect.canonical_place !=
                        statement.receiver + "." + statement.field ||
                    effect.source_offset != statement.offset)
                throw std::runtime_error(
                    "ownership violation: source reinit checker inventory mismatch");
        }
        for (std::size_t i = 0; i < function.statements.size(); ++i)
            if (function.statements[i].compiler_checked_field_reinitialization &&
                    !checked_effects.count(i))
                throw std::runtime_error(
                    "ownership violation: source reinit marker lacks checker operation");
    }
    if (expected_node != program.compiler_source_node_count)
        throw std::runtime_error(
            "ownership violation: source node inventory truncated or extended");
}


// This is the only supported post-validation source *structural* mutation:
// compiler-owned tree shaking has already compiled and checked the original
// complete Program and supplies a set of retained, previously sealed source
// function IDs. Reissue a fresh compilation unit epoch and all AST paths
// atomically with respect to codegen admission. Never apply to user-defined
// unvalidated or partially mutated source.
inline void prune_and_reseal_validated_functions(
        Program& program, const std::set<std::uint64_t>& retained_ids) {
    validate_program_identity(program); // reject stale/mutated input first
    if (retained_ids.empty() ||
            retained_ids.size() > program.functions.size())
        throw std::runtime_error(
            "ownership violation: compiler tree shake has invalid retained source IDs");
    for (const auto id : retained_ids)
        if (!id || id > program.compiler_source_function_count)
            throw std::runtime_error(
                "ownership violation: compiler tree shake references a foreign source function ID");

    try {
        program.functions.erase(
            std::remove_if(program.functions.begin(), program.functions.end(),
                [&](const Function& function) {
                    return !retained_ids.count(
                        function.compiler_source_function_id);
                }),
            program.functions.end());
        if (program.functions.size() != retained_ids.size())
            throw std::runtime_error(
                "ownership violation: compiler tree shake retained-function inventory changed");

        const auto reset_tree = [&](const auto& self, Statement& statement) -> void {
            statement.compiler_source_node_id = 0;
            statement.compiler_source_path.clear();
            // Retain the actual previously checked field reinit marker, but
            // reissue the source-local write identity for this new epoch.
            statement.compiler_reinitialize_write_id = 0;
            for (auto& child : statement.then_statements) self(self, child);
            for (auto& child : statement.else_statements) self(self, child);
        };
        for (auto& function : program.functions) {
            function.compiler_source_function_id = 0;
            function.compiler_source_sealed_name.clear();
            for (auto& effect : function.compiler_checked_reinitialize_operations)
                effect.sealed_write_id = 0;
            for (auto& stmt : function.statements) reset_tree(reset_tree, stmt);
            for (auto& stmt : function.caught_throw_defers) reset_tree(reset_tree, stmt);
        }
        program.compiler_source_unit_id = 0;
        program.compiler_source_function_count = 0;
        program.compiler_source_node_count = 0;
        seal_validated_program(program);
        validate_program_identity(program);
    } catch (...) {
        // Failed pruning/resealing must not leave a previously authorized
        // Program eligible for codegen, even if a Future pipeline had
        // been admitted before a future compiler tree-shake extension.
        program.compiler_source_unit_id = 0;
        for (auto& function : program.functions)
            for (auto& pipeline : function.future_pipelines)
                pipeline.ownership_admitted = false;
        throw;
    }
}

} // namespace oreslang::source_identity
