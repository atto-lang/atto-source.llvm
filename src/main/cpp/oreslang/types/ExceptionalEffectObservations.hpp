#pragma once

// Bounded, compiler-AST-derived effect observations for the native compiler.
// These are soundly labeled OBSERVATIONS, not typed escaping-effect proofs.
// In particular, a locally caught throw is still an observed throw, and an
// unannotated function is never inferred exception-free.
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang {

struct ExceptionalEffectObservation {
    std::string identity;
    FunctionEffects direct{};
    FunctionEffects transitive{};
    bool unproved_direct = false;

    bool has_observed(FunctionEffect effect) const noexcept {
        const auto bit = static_cast<std::uint32_t>(effect);
        // Unresolved is an intentionally invalid bit in FunctionEffects.
        // A real observed throw must not disappear when unknown is also set.
        return bit != 0 && (bit & (bit - 1u)) == 0 &&
            (bit & FunctionEffects::KnownBits) != 0 &&
            (transitive.bits & bit) != 0;
    }
    bool has_unknown_call() const noexcept {
        return (transitive.bits & FunctionEffects::UnresolvedCallBit) != 0;
    }
};

struct ExceptionalEffectObservations {
    std::vector<ExceptionalEffectObservation> functions;
    std::size_t scanned_edges = 0;
    std::size_t unresolved_edges = 0;
    bool budget_exceeded = false;
    bool ambiguous_callables = false;
    bool unmodeled_program = false;

    // Deliberately absent: a "proved_no_effects" or "success" flag.
    // This scanner is not a typed exceptional effect analyzer.
    const ExceptionalEffectObservation* find(std::string_view identity) const {
        for (const auto& function : functions)
            if (function.identity == identity) return &function;
        return nullptr;
    }

    std::uint32_t observed_exception_bits() const noexcept {
        constexpr std::uint32_t exceptional =
            static_cast<std::uint32_t>(FunctionEffect::Throw) |
            static_cast<std::uint32_t>(FunctionEffect::Raise) |
            static_cast<std::uint32_t>(FunctionEffect::Panic);
        std::uint32_t bits = 0;
        for (const auto& function : functions)
            bits |= function.transitive.bits;
        return bits & exceptional;
    }

    bool has_unknown() const noexcept {
        if (budget_exceeded || ambiguous_callables || unmodeled_program ||
            unresolved_edges != 0) return true;
        for (const auto& function : functions)
            if (function.has_unknown_call()) return true;
        return false;
    }
};

inline ExceptionalEffectObservations observe_exceptional_effects(const Program& program) {
    ExceptionalEffectObservations report;
    // Actors, module init, foreign imports and other program roots are not
    // represented by this ordinary-function graph; never certify coverage.
    report.unmodeled_program = !program.actors.empty() ||
        !program.imports.empty() || !program.classes.empty() ||
        !program.cooperative_loops.empty() || !program.generators.empty();
    const auto count = program.functions.size();
    if (count > realtime_detail::kMaxRealtimeFunctions) {
        report.budget_exceeded = true;
        return report;
    }

    std::unordered_map<std::string, std::size_t> unique;
    unique.reserve(count);
    std::vector<realtime_detail::DirectFacts> facts;
    facts.reserve(count);
    report.functions.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& fn = program.functions[i];
        const std::string identity =
            realtime_detail::function_key(fn.name, fn.parameters.size());
        if (!unique.emplace(identity, i).second)
            report.ambiguous_callables = true;
        const std::size_t remaining =
            realtime_detail::kMaxRealtimeCallEdges - report.scanned_edges;
        auto direct = realtime_detail::direct_facts(fn, remaining, &program);
        // Never consume a truncated traversal as a partial clean summary.
        if (direct.call_budget_exceeded) {
            report.budget_exceeded = true;
            return report;
        }
        report.scanned_edges += direct.calls.size();
        const bool unproved = direct.unsupported_runtime ||
            !direct.bounded_calls || !direct.bounded_cleanup ||
            !fn.method_owner.empty();
        FunctionEffects initial = direct.effects;
        if (unproved) initial.bits |= FunctionEffects::UnresolvedCallBit;
        report.functions.push_back(
            {identity, direct.effects, initial, unproved});
        facts.push_back(std::move(direct));
    }

    // A method-family name/arity without its owner is not a unique callable
    // authority. Preserve the observations, but do not resolve calls through
    // an arbitrary member of a colliding family.
    std::vector<std::vector<std::size_t>> callers(count);
    for (std::size_t from = 0; from < count; ++from) {
        for (const auto& target : facts[from].calls) {
            const auto found = unique.find(target);
            if (found == unique.end() || report.ambiguous_callables) {
                ++report.unresolved_edges;
                report.functions[from].transitive.bits |=
                    FunctionEffects::UnresolvedCallBit;
            } else {
                callers[found->second].push_back(from);
            }
        }
    }

    // Monotone reverse-edge fixed point. Recursion is allowed for observation:
    // no effect can be erased by a back edge, and an unknown callee remains
    // unknown through all its callers. Each of the 11 possible bits is added
    // at most once per callable, so this terminates on bounded graphs.
    std::deque<std::size_t> pending;
    std::vector<bool> queued(count, true);
    for (std::size_t i = 0; i < count; ++i) pending.push_back(i);
    while (!pending.empty()) {
        const auto callee = pending.front();
        pending.pop_front();
        queued[callee] = false;
        const auto transitive = report.functions[callee].transitive.bits;
        for (const auto caller : callers[callee]) {
            auto& bits = report.functions[caller].transitive.bits;
            const auto joined = bits | transitive;
            if (joined != bits) {
                bits = joined;
                if (!queued[caller]) {
                    queued[caller] = true;
                    pending.push_back(caller);
                }
            }
        }
    }

    return report;
}

} // namespace oreslang
