#pragma once

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::types {

// Strict native signed-i64 interval pre/post checker. This helper deliberately
// does not prove arbitrary predicates, full CFG return coverage or codegen.
struct NativeI64Interval {
    std::int64_t minimum;
    std::int64_t maximum;
    NativeI64Interval(std::int64_t lower, std::int64_t upper)
        : minimum(lower), maximum(upper) {
        if (lower > upper) throw std::invalid_argument("empty native refinement interval");
    }
    bool entails(const NativeI64Interval& desired) const noexcept {
        return minimum >= desired.minimum && maximum <= desired.maximum;
    }
};

enum class NativeRefinementEffect {
    Io, Actor, Throw, Raise, Panic, Cancellation, FfiUnknown, UnknownCall
};
struct NativeFunctionRefinement {
    std::string resolved_symbol;
    std::vector<NativeI64Interval> parameters;
    NativeI64Interval result{0, 0};
    std::set<NativeRefinementEffect> allowed_effects;
};

inline void require_native_function_refinement_contract(
        const NativeFunctionRefinement& contract) {
    if (contract.resolved_symbol.empty() ||
            contract.allowed_effects.count(NativeRefinementEffect::FfiUnknown) ||
            contract.allowed_effects.count(NativeRefinementEffect::UnknownCall))
        throw std::invalid_argument("unknown function identity/effect in refinement contract");
}
inline void require_native_refined_call(
        const NativeFunctionRefinement& contract,
        const std::vector<NativeI64Interval>& actual_args,
        const std::set<NativeRefinementEffect>& inferred_effects) {
    require_native_function_refinement_contract(contract);
    if (actual_args.size() != contract.parameters.size())
        throw std::invalid_argument("refinement contract arity mismatch");
    for (std::size_t i = 0; i < actual_args.size(); ++i)
        if (!actual_args[i].entails(contract.parameters[i]))
            throw std::invalid_argument("unproved native refinement precondition");
    for (const auto effect : inferred_effects)
        if (!contract.allowed_effects.count(effect))
            throw std::invalid_argument("unproved native callable effect");
}
inline void require_native_refined_returns(
        const NativeFunctionRefinement& contract,
        const std::vector<NativeI64Interval>& all_normal_returns) {
    require_native_function_refinement_contract(contract);
    if (all_normal_returns.empty())
        throw std::invalid_argument("return CFG coverage must be proven separately");
    for (const auto& actual : all_normal_returns)
        if (!actual.entails(contract.result))
            throw std::invalid_argument("unproved native refinement postcondition");
}

} // namespace oreslang::types
