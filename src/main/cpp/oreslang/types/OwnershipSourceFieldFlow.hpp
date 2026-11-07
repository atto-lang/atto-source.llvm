#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipCfg.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::ownership {

// Source-AST-derived, path-sensitive companion to the existing return CFG.
// Activated only when the ownership checker minted a restoration ticket.
// This is a bounded, closed-world *field-state* proof, not a loan-use/SSA
// completeness certificate, optimized LLVM dominator tree or guest authority.
struct SourceFieldFlow {
    Graph graph;
    std::set<std::uint64_t> reinitialization_sites;
    bool source_loan_use_complete = false;
    bool optimized_llvm_dominance_verified = false;
    bool guest_verified = false;
};

class SourceFieldFlowBuilder {
    static constexpr std::size_t kMaxBlocks = 2048;
    static constexpr std::size_t kMaxOperations = 16384;
    static constexpr std::size_t kMaxNodes = 16384;
    static constexpr std::size_t kMaxDepth = 64;

    SourceFieldFlow result_;
    const Function& function_;
    const std::set<std::string> class_roots_;
    std::size_t operations_ = 0;
    std::size_t scanned_nodes_ = 0;
    std::vector<const Statement*> defers_;

    void collect_declared_sites(const std::vector<Statement>& stmts,
            std::set<std::uint64_t>& sites, std::size_t depth) {
        require_depth(depth);
        for (const auto& stmt : stmts) {
            if (++scanned_nodes_ > kMaxNodes)
                unsupported("source statement quota exceeded");
            if (stmt.reinitialize_write_id &&
                    !sites.insert(stmt.reinitialize_write_id).second)
                unsupported("repeated restoration ticket in source AST");
            collect_declared_sites(stmt.then_statements, sites, depth + 1);
            collect_declared_sites(stmt.else_statements, sites, depth + 1);
        }
    }

    [[noreturn]] static void unsupported(const std::string& what) {
        throw std::runtime_error(
            "ownership violation: source field CFG needs classified effect: " + what);
    }

    std::size_t block() {
        if (result_.graph.blocks.size() >= kMaxBlocks)
            unsupported("block quota exceeded");
        const auto index = result_.graph.blocks.size();
        result_.graph.blocks.emplace_back();
        return index;
    }
    void edge(std::size_t source, std::size_t destination) {
        if (source >= result_.graph.blocks.size() ||
                destination >= result_.graph.blocks.size())
            unsupported("invalid source CFG edge");
        auto& next = result_.graph.blocks[source].successors;
        if (next.size() >= 2) unsupported("unbounded CFG fanout");
        next.push_back(destination);
    }
    void emit(std::size_t at, Operation::Kind kind,
            const std::string& place,
            std::optional<std::uint64_t> write_id = std::nullopt) {
        if (++operations_ > kMaxOperations)
            unsupported("operation quota exceeded");
        result_.graph.blocks.at(at).operations.emplace_back(
            kind, place, std::nullopt, write_id);
    }
    static void require_depth(std::size_t depth) {
        if (depth > kMaxDepth)
            unsupported("nested source expression/statement limit exceeded");
    }
    void place_read(std::size_t at, const std::string& root,
            const std::string& field) {
        const std::string key = root + "." + field;
        if (!result_.graph.entry.count(key))
            unsupported("unresolved tracked class field " + key);
        emit(at, Operation::Kind::Read, key);
    }

    // Expression evaluation order is explicit. For a conditional or
    // short-circuit expression, create real branch edges, not a flattened
    // optimistic sequence of reads or moves.
    std::size_t expression(const Expression& expr, std::size_t at,
            std::size_t depth) {
        require_depth(depth);
        using K = Expression::Kind;
        switch (expr.kind) {
            case K::Integer:
            case K::Boolean:
            case K::String:
                return at;
            case K::Variable:
                if (class_roots_.count(expr.name))
                    emit(at, Operation::Kind::ReadObject, expr.name);
                return at;
            case K::Field: {
                if (!expr.left || expr.left->kind != K::Variable)
                    unsupported("nested or untracked field projection");
                if (!class_roots_.count(expr.left->name))
                    unsupported("field read from untracked source owner");
                place_read(at, expr.left->name, expr.name);
                return at;
            }
            case K::Take:
                if (!expr.left)
                    unsupported("source take missing operand");
                if (expr.left->kind == K::Variable) {
                    if (class_roots_.count(expr.left->name))
                        emit(at, Operation::Kind::MoveObject, expr.left->name);
                    return at;
                }
                if (expr.left->kind == K::Field &&
                        expr.left->left &&
                        expr.left->left->kind == K::Variable) {
                    const auto& root = expr.left->left->name;
                    if (!class_roots_.count(root))
                        unsupported("field take from untracked source owner");
                    const auto key = root + "." + expr.left->name;
                    if (!result_.graph.entry.count(key))
                        unsupported("take from unknown class field " + key);
                    emit(at, Operation::Kind::Move, key);
                    return at;
                }
                unsupported("opaque take source");
            case K::Copy:
            case K::Negate:
            case K::LogicalNot:
                if (!expr.left) unsupported("unary source expression missing operand");
                return expression(*expr.left, at, depth + 1);
            case K::Add:
            case K::Subtract:
            case K::Multiply:
            case K::Remainder:
            case K::Equal:
            case K::NotEqual:
            case K::Less:
            case K::LessEqual:
            case K::Greater:
            case K::GreaterEqual:
            case K::LogicalXor: {
                if (!expr.left || !expr.right)
                    unsupported("binary expression missing operand");
                const auto after_left = expression(*expr.left, at, depth + 1);
                return expression(*expr.right, after_left, depth + 1);
            }
            case K::LogicalAnd:
            case K::LogicalOr: {
                if (!expr.left || !expr.right)
                    unsupported("short-circuit expression missing operand");
                const auto after_left = expression(*expr.left, at, depth + 1);
                const auto short_path = block();
                const auto evaluate_rhs = block();
                const auto join = block();
                edge(after_left, short_path);
                edge(after_left, evaluate_rhs);
                edge(short_path, join);
                edge(expression(*expr.right, evaluate_rhs, depth + 1), join);
                return join;
            }
            case K::Conditional: {
                if (!expr.when_true || !expr.when_false)
                    unsupported("incomplete conditional expression");
                std::size_t pred = at;
                if (expr.condition)
                    pred = expression(*expr.condition, pred, depth + 1);
                else {
                    if (!expr.condition_lhs || !expr.condition_rhs)
                        unsupported("conditional expression without predicate");
                    pred = expression(*expr.condition_lhs, pred, depth + 1);
                    pred = expression(*expr.condition_rhs, pred, depth + 1);
                }
                const auto yes = block(), no = block(), join = block();
                edge(pred, yes);
                edge(pred, no);
                edge(expression(*expr.when_true, yes, depth + 1), join);
                edge(expression(*expr.when_false, no, depth + 1), join);
                return join;
            }
            // A call/borrow/suspension can hide effects or borrowed-use
            // origins. A complete typed SSA pass is required to admit it.
            case K::Call:
            case K::ArrayCall:
            case K::Console:
            case K::RecordConstruct:
            case K::SumConstruct:
            case K::SumAccess:
            case K::Share:
            case K::Borrow:
            case K::BorrowMut:
                unsupported("opaque call/borrow/aggregate expression");
        }
        unsupported("unknown source expression classification");
    }

    std::size_t field_write(const Statement& stmt, std::size_t at) {
        if (!stmt.expression) unsupported("field assignment without expression");
        at = expression(*stmt.expression, at, 1);
        const auto key = stmt.receiver + "." + stmt.field;
        const bool tracked = class_roots_.count(stmt.receiver) != 0;
        if (!tracked)
            unsupported("source field write from untracked class owner");
        if (!result_.graph.entry.count(key))
            unsupported("field write to unknown tracked projection " + key);
        if (stmt.reinitialize_write_id) {
            const auto ticket = stmt.reinitialize_write_id;
            constexpr std::uint64_t high = std::uint64_t{1} << 63;
            if (!(ticket & high) || !(ticket & ~high) ||
                    (ticket & ~high) > 4096 ||
                    !result_.reinitialization_sites.insert(ticket).second)
                unsupported("forged or duplicate source restoration ticket");
            emit(at, Operation::Kind::Reinitialize, key, ticket);
        } else {
            // Ordinary field overwrites require definitely Owned on every
            // incoming path. An unrecorded restoration cannot silently heal
            // a moved field even if the LLVM backend emits a physical store.
            emit(at, Operation::Kind::Read, key);
        }
        return at;
    }

    std::optional<std::size_t> cleanup_and_exit(std::size_t at,
            const Expression* value) {
        if (value) at = expression(*value, at, 1);
        for (auto it = defers_.rbegin(); it != defers_.rend(); ++it) {
            const auto& stmt = **it;
            if (!stmt.receiver.empty())
                at = field_write(stmt, at);
            else
                unsupported("opaque deferred call");
        }
        // No successor: execution really terminates here.
        return std::nullopt;
    }

    std::optional<std::size_t> statement(const Statement& stmt,
            std::size_t at, std::size_t depth) {
        require_depth(depth);
        using SK = Statement::Kind;
        switch (stmt.kind) {
            case SK::FieldWrite:
                return field_write(stmt, at);
            case SK::ScalarWrite:
            case SK::Call:
                if (!stmt.expression) unsupported("opaque statement without expression");
                return expression(*stmt.expression, at, depth + 1);
            case SK::Return:
                return cleanup_and_exit(at, stmt.expression.get());
            case SK::ScopedLocal:
                if (!stmt.branch_local || !stmt.branch_local->class_name.empty())
                    unsupported("unclassified scoped class owner");
                if (stmt.branch_local->initializer)
                    at = expression(*stmt.branch_local->initializer, at, depth + 1);
                return at;
            case SK::Conditional: {
                if (stmt.condition)
                    at = expression(*stmt.condition, at, depth + 1);
                else {
                    if (!stmt.condition_lhs || !stmt.condition_rhs)
                        unsupported("if statement without predicate");
                    at = expression(*stmt.condition_lhs, at, depth + 1);
                    at = expression(*stmt.condition_rhs, at, depth + 1);
                }
                const auto yes = block(), no = block(), join = block();
                edge(at, yes);
                edge(at, no);
                const auto yes_end = statements(stmt.then_statements,
                    yes, depth + 1);
                const auto no_end = statements(stmt.else_statements,
                    no, depth + 1);
                if (yes_end) edge(*yes_end, join);
                if (no_end) edge(*no_end, join);
                if (!yes_end && !no_end) return std::nullopt;
                return join;
            }
            case SK::ForIndex:
            case SK::ForEach: {
                if (stmt.kind == SK::ForIndex) {
                    if (!stmt.branch_local || !stmt.branch_local->initializer ||
                            !stmt.branch_local->class_name.empty() ||
                            stmt.branch_local->record_view ||
                            !stmt.condition || !stmt.loop_step)
                        unsupported("unclassified index-loop class owner or missing frame");
                    at = expression(*stmt.branch_local->initializer, at, depth + 1);
                } else {
                    if (!stmt.expression || !stmt.branch_local ||
                            !stmt.branch_local->class_name.empty() ||
                            stmt.branch_local->record_view)
                        unsupported("foreach has untracked borrowed class element or source");
                    at = expression(*stmt.expression, at, depth + 1);
                }
                const auto header = block(), body = block(), exit = block();
                edge(at, header);
                std::size_t condition_end = header;
                if (stmt.kind == SK::ForIndex)
                    condition_end = expression(*stmt.condition, header, depth + 1);
                else
                    condition_end = expression(*stmt.expression, header, depth + 1);
                edge(condition_end, body);
                edge(condition_end, exit);
                const auto body_end = statements(stmt.then_statements,
                    body, depth + 1);
                if (body_end) {
                    std::size_t tail = *body_end;
                    if (stmt.kind == SK::ForIndex)
                        tail = expression(*stmt.loop_step, tail, depth + 1);
                    edge(tail, header);
                }
                return exit;
            }
            case SK::Defer:
                unsupported("nested or dynamically registered defer");
            case SK::ArrayCommitFilled:
                unsupported("native array commit has hidden multiple field writes");
        }
        unsupported("unknown source statement classification");
    }
    std::optional<std::size_t> statements(
            const std::vector<Statement>& source,
            std::size_t current, std::size_t depth) {
        require_depth(depth);
        std::optional<std::size_t> next = current;
        for (const auto& stmt : source) {
            if (!next) {
                // Dead source tail cannot add restoration evidence later.
                if (stmt.reinitialize_write_id)
                    unsupported("restoration ticket on unreachable statement");
                continue;
            }
            next = statement(stmt, *next, depth + 1);
        }
        return next;
    }

public:
    SourceFieldFlowBuilder(const Function& source, State initial,
            std::set<std::string> class_roots)
        : function_(source), class_roots_(std::move(class_roots)) {
        result_.graph.entry = std::move(initial);
        if (result_.graph.entry.empty() || class_roots_.empty())
            unsupported("missing tracked class field state");
        validate_disjoint_place_layout(result_.graph.entry);
    }

    SourceFieldFlow build() {
        if (function_.caught_throw_value ||
                !function_.caught_throw_defers.empty() ||
                !function_.actor_actions.empty() ||
                !function_.actor_field_writes.empty() ||
                !function_.actor_spawns.empty() ||
                !function_.actor_sends.empty() ||
                !function_.actor_stops.empty() ||
                function_.async_source_i64 ||
                !function_.source_tasks.empty() ||
                !function_.future_pipelines.empty() ||
                !function_.ready_await_actor.empty() ||
                !function_.done_await_actor.empty())
            unsupported("exception/actor/task/suspension exits not modeled");
        std::set<std::uint64_t> declared_sites;
        collect_declared_sites(function_.statements, declared_sites, 0);
        collect_declared_sites(function_.caught_throw_defers, declared_sites, 0);
        if (declared_sites.empty())
            unsupported("missing compiler-minted restoration witness");
        const auto start = block();
        std::optional<std::size_t> current = start;
        for (const auto& stmt : function_.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                if (!current || !stmt.expression ||
                        defers_.size() >= 128)
                    unsupported("invalid or unreachable defer");
                defers_.push_back(&stmt);
                continue;
            }
            if (current) current = statement(stmt, *current, 1);
            else if (stmt.reinitialize_write_id)
                unsupported("unreachable source restoration ticket");
        }
        for (const auto& guard : function_.early_returns) {
            if (!current) break;
            std::size_t at = *current;
            if (guard.condition)
                at = expression(*guard.condition, at, 1);
            else {
                if (!guard.lhs || !guard.rhs)
                    unsupported("early-return guard missing operands");
                at = expression(*guard.lhs, at, 1);
                at = expression(*guard.rhs, at, 1);
            }
            const auto taken = block(), next = block();
            edge(at, taken);
            edge(at, next);
            (void)cleanup_and_exit(taken, guard.result.get());
            current = next;
        }
        if (current) (void)cleanup_and_exit(*current, function_.result.get());
        if (result_.reinitialization_sites != declared_sites)
            unsupported("unreachable or omitted source restoration ticket");
        // The dataflow verifier computes the powerset of possible ownership
        // states at every join/backedge: a read requires kOwned, not
        // {kOwned,kMoved}. This enforces all-predecessor restoration coverage
        // without pretending an individual branch store dominates the merge.
        (void)verify(result_.graph);
        return std::move(result_);
    }
};

inline SourceFieldFlow verify_source_field_flow(
        const Function& fn, State initial,
        std::set<std::string> class_roots) {
    return SourceFieldFlowBuilder(fn, std::move(initial),
        std::move(class_roots)).build();
}

} // namespace oreslang::ownership
