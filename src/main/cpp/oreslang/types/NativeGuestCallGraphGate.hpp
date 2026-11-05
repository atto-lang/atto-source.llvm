#pragma once

#include "oreslang/types/RealtimeAnalysis.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace oreslang {

/**
 * An intentionally narrow *candidate* probe: only an expression returned
 * immediately from a function, or selected conditional arm, is a possible
 * scalar tail transfer. A call buried in arithmetic or in another call's
 * arguments is not in tail position.
 *
 * Finding a candidate never authorizes LLVM musttail. ABI equivalence,
 * owned cleanup, EH continuations, scheduler safepoints and the existing
 * native frame budget must be proved separately by the actual emitter.
 */
class NativeScalarTailPositionProbe final {
public:
    struct Candidates {
        std::size_t direct_return_sites = 0;
        std::size_t conditional_arms = 0;
    };
    static Candidates inspect(const Expression* returned) {
        Candidates out;
        if (!returned) return out;
        std::vector<const Expression*> pending{returned};
        constexpr std::size_t max_nodes = 4096;
        std::size_t visited = 0;
        while (!pending.empty()) {
            const Expression* current = pending.back();
            pending.pop_back();
            if (!current) throw std::runtime_error("malformed scalar tail expression");
            if (++visited > max_nodes)
                throw std::runtime_error("scalar tail-position inspection budget exceeded");
            if (current->kind == Expression::Kind::Call) {
                ++out.direct_return_sites;
                // Arguments are evaluated before a tail transfer and are
                // never themselves tail calls of the enclosing function.
            } else if (current->kind == Expression::Kind::Conditional) {
                if (!current->when_true || !current->when_false)
                    throw std::runtime_error("malformed conditional tail arms");
                ++out.conditional_arms;
                pending.push_back(current->when_false.get());
                pending.push_back(current->when_true.get());
            }
            // Everything else, especially addition, logical operators and
            // nested argument expression calls, is non-tail at this root.
        }
        return out;
    }
};


/**
 * Temporary native fail-closed admission until compiler-managed bounded
 * guest frames exist. An OS carrier stack is NOT an actor's allocation heap.
 *
 * Unlike Java's M0 runtime guard, this gate prevents the current native LLVM
 * direct-call lowering from emitting an unbounded recursive cycle at all.
 * It does not turn StackOverflow/SIGSEGV into a recoverable actor failure.
 * Never replace rejection with llvm.trap/abort/longjmp on a shared carrier.
 *
 * All source callable identities use name/arity; no host stack pointer,
 * actor reference, source text or user-controlled path is retained here.
 */
class NativeGuestCallGraphGate final {
public:
    static constexpr std::size_t max_functions = 4096;
    static constexpr std::size_t max_edges = 16384;
    static constexpr std::size_t max_depth = 64;
    using Graph = std::unordered_map<std::string, std::vector<std::string>>;

    struct Inspection {
        std::size_t peak = 0;
        bool recursive = false;
    };

    // A cyclic graph may only be admitted when the emitter actually installs
    // checked frame prologues, failure propagation and a top-level failure ABI.
    // Default host-only users and non-LLVM consumers continue to reject it.
    static Inspection inspect(const Graph& graph, bool allow_recursion) {
        if (graph.size() > max_functions)
            throw std::runtime_error("native guest call graph function budget exceeded");
        std::size_t total_edges = 0;
        for (const auto& entry : graph) {
            if (entry.first.empty())
                throw std::runtime_error("native guest call graph has invalid function identity");
            if (entry.second.size() > max_edges - total_edges)
                throw std::runtime_error("native guest call graph edge budget exceeded");
            total_edges += entry.second.size();
            for (const auto& child : entry.second) {
                if (child.empty() || graph.find(child) == graph.end())
                    throw std::runtime_error("native guest call graph has unresolved target");
            }
        }

        // A cached longest path is valid regardless of its caller; each parent
        // checks its own total depth before admitting the graph.
        Inspection inspection;
        std::unordered_map<std::string, std::uint8_t> state;
        std::unordered_map<std::string, std::size_t> longest;
        std::function<std::size_t(const std::string&, std::size_t)> visit =
                [&](const std::string& key, std::size_t depth) -> std::size_t {
            if (depth > max_depth)
                throw std::runtime_error("native guest call depth budget exceeded");
            const auto status = state.find(key);
            if (status != state.end()) {
                if (status->second == 1) {
                    if (!allow_recursion)
                        throw std::runtime_error("native recursive call graph requires bounded guest frames");
                    inspection.recursive = true;
                    return 0; // The real runtime frame guard bounds this back edge.
                }
                if (status->second == 2) return longest.at(key);
            }
            state[key] = 1;
            std::size_t span = 1;
            for (const auto& child : graph.at(key)) {
                const auto child_span = visit(child, depth + 1);
                if (child_span >= max_depth)
                    throw std::runtime_error("native guest call depth budget exceeded");
                span = std::max(span, child_span + 1);
            }
            state[key] = 2;
            longest.emplace(key, span);
            return span;
        };
        // All functions, not just main: host-callable public entries and
        // unused callbacks cannot bypass this conservative safety gate.
        for (const auto& entry : graph)
            inspection.peak = std::max(inspection.peak, visit(entry.first, 1));
        return inspection;
    }

    static std::size_t require_bounded(const Graph& graph) {
        return inspect(graph, false).peak;
    }
};

/**
 * Scan all direct expression call edges BEFORE tree shaking or LLVM emission.
 * Reuse the native real-time AST walker so conditional arms, nested locals,
 * deferred cleanup and constructor default expressions are not skipped.
 */
/**
 * Initial *executable* LLVM recursion slice. A recursive function can fail
 * without side effects only when the entire program is a closed scalar
 * expression graph. Do not admit actors, defers, ownership, host IO, task
 * suspension or other paths until cleanup-safe failure ABI is implemented.
 */
inline void require_native_scalar_frame_subset(const Program& source) {
    if (!source.actors.empty() || !source.classes.empty() ||
            !source.sum_types.empty() || !source.imports.empty() ||
            !source.generators.empty() || !source.cooperative_loops.empty()) {
        throw std::runtime_error(
            "native bounded recursion requires scalar functions without actors or owned state");
    }
    // A 64-call counter is NOT a bound on bytes consumed by native ABI
    // frames. Until target-specific stack usage is proven, constrain argument
    // spills and LLVM O0 expression temporaries as well as call depth.
    constexpr std::size_t max_scalar_parameters_per_frame = 8;
    constexpr std::size_t max_scalar_expressions_per_frame = 128;
    constexpr std::size_t max_scalar_expressions_per_unit = 4096;
    std::size_t total_expression_nodes = 0;
    bool has_main = false;
    for (const auto& fn : source.functions) {
        if (fn.parameters.size() > max_scalar_parameters_per_frame)
            throw std::runtime_error(
                "native bounded recursion per-frame parameter budget exceeded");
        if (fn.name == "main") {
            if (!fn.parameters.empty() || has_main)
                throw std::runtime_error(
                    "native bounded recursion requires exactly one main/0 entry");
            has_main = true;
        }
        if (fn.name.rfind("__atto_", 0) == 0)
            throw std::runtime_error(
                "native bounded recursion reserves compiler frame ABI names");
        if (fn.returns_void || fn.return_type != ScalarType::I64 ||
                !fn.result || !fn.locals.empty() || !fn.statements.empty() ||
                !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
                !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
                !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
                fn.async_source_i64 || !fn.ready_await_actor.empty() ||
                !fn.done_await_actor.empty() || !fn.ready_future_name.empty() ||
                !fn.ready_future_actor.empty() || fn.ready_await_form != NativeAwaitForm::None ||
                !fn.return_class.empty() || !fn.return_sum.empty() ||
                fn.caught_throw_value || !fn.caught_throw_defers.empty() ||
                fn.scalar_while || fn.native_internal ||
                fn.has_declared_effects || !fn.generic_parameters.empty() ||
                !fn.method_owner.empty()) {
            throw std::runtime_error(
                "native bounded recursion requires pure scalar expression-only functions");
        }
        // Host ABI entry is only OS main. Host-callable helper code is
        // prohibited until reentrant per-invocation frame ownership exists.
        if (fn.exported && fn.name != "main")
            throw std::runtime_error(
                "native bounded recursion rejects exported helper ABI entry");
        for (const auto& param : fn.parameters) {
            if (param.mode != Parameter::Mode::Value ||
                    param.scalar_type != ScalarType::I64 ||
                    !param.class_name.empty() || !param.generic_type.empty() ||
                    !param.sum_type.empty()) {
                throw std::runtime_error(
                    "native bounded recursion requires by-value int parameters");
            }
        }
        std::vector<const Expression*> pending{fn.result.get()};
        std::size_t visited = 0;
        while (!pending.empty()) {
            const Expression* expr = pending.back();
            pending.pop_back();
            if (!expr) continue;
            if (++visited > max_scalar_expressions_per_frame ||
                    ++total_expression_nodes > max_scalar_expressions_per_unit ||
                    expr->type_arguments_present ||
                    !expr->type_arguments.empty()) {
                throw std::runtime_error(
                    "native bounded recursion per-frame expression budget exceeded");
            }
            if (expr->kind == Expression::Kind::Call &&
                    expr->arguments.size() > max_scalar_parameters_per_frame) {
                throw std::runtime_error(
                    "native bounded recursion call arity budget exceeded");
            }
            switch (expr->kind) {
                case Expression::Kind::Integer:
                case Expression::Kind::Boolean:
                case Expression::Kind::Variable:
                case Expression::Kind::Negate:
                case Expression::Kind::LogicalNot:
                case Expression::Kind::Add:
                case Expression::Kind::Subtract:
                case Expression::Kind::Multiply:
                case Expression::Kind::Equal:
                case Expression::Kind::NotEqual:
                case Expression::Kind::Less:
                case Expression::Kind::LessEqual:
                case Expression::Kind::Greater:
                case Expression::Kind::GreaterEqual:
                case Expression::Kind::LogicalAnd:
                case Expression::Kind::LogicalOr:
                case Expression::Kind::LogicalXor:
                case Expression::Kind::Conditional:
                case Expression::Kind::Call:
                    break;
                default:
                    throw std::runtime_error(
                        "native bounded recursion forbids side effects and owned expressions");
            }
            if (expr->left) pending.push_back(expr->left.get());
            if (expr->right) pending.push_back(expr->right.get());
            if (expr->condition) pending.push_back(expr->condition.get());
            if (expr->condition_lhs) pending.push_back(expr->condition_lhs.get());
            if (expr->condition_rhs) pending.push_back(expr->condition_rhs.get());
            if (expr->when_true) pending.push_back(expr->when_true.get());
            if (expr->when_false) pending.push_back(expr->when_false.get());
            for (const auto& arg : expr->arguments)
                if (arg) pending.push_back(arg.get());
        }
    }
    if (!has_main)
        throw std::runtime_error(
            "native bounded recursion requires a private OS-main entrypoint");
}

/** Return true only when checked LLVM scalar-frame lowering is required. */
inline bool require_native_guest_call_graph_bounded(
        const Program& source, bool allow_checked_scalar_recursion = false) {
    NativeGuestCallGraphGate::Graph graph;
    std::size_t edges = 0;
    auto add = [&](const std::string& identity,
                   realtime_detail::DirectFacts facts) {
        if (facts.call_budget_exceeded ||
                facts.calls.size() > NativeGuestCallGraphGate::max_edges - edges)
            throw std::runtime_error("native guest call graph edge budget exceeded");
        edges += facts.calls.size();
        if (!graph.emplace(identity, std::move(facts.calls)).second)
            throw std::runtime_error("native guest callable identity collision");
        if (graph.size() > NativeGuestCallGraphGate::max_functions)
            throw std::runtime_error("native guest call graph function budget exceeded");
    };
    auto remaining = [&]() {
        return NativeGuestCallGraphGate::max_edges - edges;
    };
    for (const auto& function : source.functions) {
        add(realtime_detail::function_key(
                    function.name, function.parameters.size()),
            realtime_detail::direct_facts(function, remaining(), &source));
    }
    // Actor handlers/init fields are distinct runtime-generated ABI roots,
    // not ordinary fnc declarations. Do not leave their direct calls unscanned.
    for (std::size_t index = 0; index < source.actors.size(); ++index) {
        const auto& actor = source.actors[index];
        add("$atto.actor.handler." + std::to_string(index),
            realtime_detail::direct_facts(actor.on_message, remaining(), &source));
        realtime_detail::DirectFacts initializers;
        initializers.call_limit = remaining();
        for (const auto& field : actor.fields) {
            realtime_detail::scan_binding(field, initializers);
            if (initializers.call_budget_exceeded) break;
            if (field.new_class) {
                realtime_detail::scan_constructor_fields(
                    field, source, initializers);
                if (initializers.call_budget_exceeded) break;
            }
        }
        add("$atto.actor.fields." + std::to_string(index),
            std::move(initializers));
    }
    // Conservative treatment of nominal field defaults, including defaults
    // materialized by a native record constructor outside an explicit local.
    for (std::size_t index = 0; index < source.classes.size(); ++index) {
        realtime_detail::DirectFacts defaults;
        defaults.call_limit = remaining();
        for (const auto& field : source.classes[index].fields) {
            realtime_detail::scan_expression(
                field.initializer_expression.get(), defaults);
            if (defaults.call_budget_exceeded) break;
        }
        add("$atto.class.fields." + std::to_string(index),
            std::move(defaults));
    }
    const auto inspection =
        NativeGuestCallGraphGate::inspect(graph, allow_checked_scalar_recursion);
    if (inspection.recursive)
        require_native_scalar_frame_subset(source);
    return inspection.recursive;
}

} // namespace oreslang
