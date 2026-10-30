#include "oreslang/runtime/HardRealtimeContract.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <iostream>

using namespace oreslang::runtime::rt;
constexpr Effects no_effects{};
constexpr TimingBudget good{
    100, 60, 10, 10, 100, 4096, SchedulingPolicy::FixedPriority
};
static_assert(static_cast<bool>(admit_native_hard_rt(no_effects, good)));
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 60, 10, 10, 0, 4096}).failure ==
    AdmissionFailure::MissingTimingBudget);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{101, 60, 10, 10, 100, 4096}).failure ==
    AdmissionFailure::MissingTimingBudget);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 60, 10, 10, 100, 0}).failure ==
    AdmissionFailure::MissingMemoryBudget);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 60, 10, 10, 100, 4096,
                 static_cast<SchedulingPolicy>(255)}).failure ==
    AdmissionFailure::UnsupportedSchedulingPolicy);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 99, 10, 10, 100, 4096}).failure ==
    AdmissionFailure::MissingTimingBudget);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 80, 10, 11, 100, 4096}).failure ==
    AdmissionFailure::MissingTimingBudget);
static_assert(admit_native_hard_rt(no_effects,
    TimingBudget{100, 60, 10, 10, 100, 4096,
                 SchedulingPolicy::EarliestDeadlineFirst}).failure ==
    AdmissionFailure::UnsupportedSchedulingPolicy);
static_assert(diagnostic_code(AdmissionFailure::MissingMemoryBudget) ==
              "RT012_MEMORY_BUDGET_UNPROVEN");
static_assert(diagnostic_code(AdmissionFailure::UnsupportedSchedulingPolicy) ==
              "RT013_UNSUPPORTED_SCHEDULER");

int main() {
    auto check = [](TimingBudget b, bool expected) {
        const auto result = admit_native_hard_rt(no_effects, b);
        if (static_cast<bool>(result) != expected)
            throw std::runtime_error("declared-bound regression");
    };
    for (std::uint64_t period : {std::uint64_t(0), std::uint64_t(1),
                                 std::uint64_t(100),
                                 std::numeric_limits<std::uint64_t>::max()}) {
        for (std::uint64_t deadline : {std::uint64_t(0), std::uint64_t(1),
                                       std::uint64_t(100),
                                       std::numeric_limits<std::uint64_t>::max()}) {
            auto budget = good;
            budget.period_ns = period;
            budget.deadline_ns = deadline;
            const bool expected = period != 0 && deadline != 0 &&
                deadline <= period && budget.wcet_ns <= deadline &&
                budget.max_blocking_ns <= deadline - budget.wcet_ns &&
                budget.max_jitter_ns <=
                    deadline - budget.wcet_ns - budget.max_blocking_ns;
            check(budget, expected);
        }
    }

    Effects deny{};
    deny.tracing_gc = true;
    if (admit_native_hard_rt(deny, good).failure !=
        AdmissionFailure::TracingGc)
        throw std::runtime_error("collector effects must fail closed");
    std::cout << "native RT declared-bound checks passed\n";
}
