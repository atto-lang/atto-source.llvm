#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::types {

// Strict native signed-i64 interval pre/post checker. This helper deliberately
// does not prove arbitrary predicates, full CFG return coverage or codegen.

// Portable C++17 checked i64 arithmetic: all overflow is detected *before*
// executing signed operations. Neither __int128 nor platform builtins are
// required; strict-AOT targets cannot rely on undefined signed overflow.
inline std::int64_t add_i64_checked(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b))
        throw std::invalid_argument("i64 interval add overflow");
    return a + b;
}
inline std::int64_t sub_i64_checked(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((b < 0 && a > hi + b) || (b > 0 && a < lo + b))
        throw std::invalid_argument("i64 interval sub overflow");
    return a - b;
}
inline std::int64_t mul_i64_checked(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((a == lo && b == -1) || (b == lo && a == -1))
        throw std::invalid_argument("i64 interval mul overflow");
    if (a > 0) {
        if ((b > 0 && a > hi / b) || (b < 0 && b < lo / a))
            throw std::invalid_argument("i64 interval mul overflow");
    } else if (a < 0) {
        if ((b > 0 && a < lo / b) || (b < 0 && a < hi / b))
            throw std::invalid_argument("i64 interval mul overflow");
    }
    return a * b;
}

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
    NativeI64Interval plus(const NativeI64Interval& other) const {
        return {add_i64_checked(minimum, other.minimum),
                add_i64_checked(maximum, other.maximum)};
    }
    NativeI64Interval minus(const NativeI64Interval& other) const {
        return {sub_i64_checked(minimum, other.maximum),
                sub_i64_checked(maximum, other.minimum)};
    }
    NativeI64Interval times(const NativeI64Interval& other) const {
        std::array<std::int64_t, 4> endpoints{
            mul_i64_checked(minimum, other.minimum),
            mul_i64_checked(minimum, other.maximum),
            mul_i64_checked(maximum, other.minimum),
            mul_i64_checked(maximum, other.maximum)
        };
        const auto bounds = std::minmax_element(endpoints.begin(), endpoints.end());
        return {*bounds.first, *bounds.second};
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

// C++ enum casts can produce values outside the defined effect vocabulary.
// Unknown bits must not be treated as effect rows that an attacker may bless.
inline bool known_native_refinement_effect(NativeRefinementEffect effect) noexcept {
    switch (effect) {
    case NativeRefinementEffect::Io:
    case NativeRefinementEffect::Actor:
    case NativeRefinementEffect::Throw:
    case NativeRefinementEffect::Raise:
    case NativeRefinementEffect::Panic:
    case NativeRefinementEffect::Cancellation:
        return true;
    case NativeRefinementEffect::FfiUnknown:
    case NativeRefinementEffect::UnknownCall:
        return false;
    }
    return false;
}
inline void require_native_function_refinement_contract(
        const NativeFunctionRefinement& contract) {
    if (contract.resolved_symbol.empty() || contract.resolved_symbol.size() > 256 ||
            std::all_of(contract.resolved_symbol.begin(), contract.resolved_symbol.end(),
                    [](unsigned char ch) { return std::isspace(ch) != 0; }))
        throw std::invalid_argument("invalid function identity in refinement contract");
    for (const auto effect : contract.allowed_effects)
        if (!known_native_refinement_effect(effect))
            throw std::invalid_argument("unknown or untrusted effect in refinement contract");
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
        if (!known_native_refinement_effect(effect) ||
                !contract.allowed_effects.count(effect))
            throw std::invalid_argument("unproved or unknown native callable effect");
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
