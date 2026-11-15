#pragma once
#include <cstdint>
#include <limits>

namespace oreslang::runtime::realtime {

enum class Requirement : std::uint64_t {
    FullGuestAot              = 1ull << 0,
    RuntimeCodegenDisabled    = 1ull << 1,
    ZeroTracingGcCriticalPath = 1ull << 2,
    DeterministicActorRegions = 1ull << 3,
    MemoryPreallocated        = 1ull << 4,
    MemoryPrefaulted          = 1ull << 5,
    BoundedDispatch           = 1ull << 6,
    BoundedPriorityInversion  = 1ull << 7,
    CriticalWorkerIsolation   = 1ull << 8,
    OsRealtimePolicy          = 1ull << 9,
    BoundedCleanup            = 1ull << 10,
    BoundedCallGraph          = 1ull << 11,
    BoundedLoops              = 1ull << 12,
    BoundedQueues             = 1ull << 13,
    BlockingIoAbsent          = 1ull << 14,
    FfiQualified              = 1ull << 15,
    UntrustedJitExcluded      = 1ull << 16,
    TimingEvidence            = 1ull << 17,
    DynamicLoadingDisabled    = 1ull << 18,
    WorkersPrestarted         = 1ull << 19
};

constexpr std::uint64_t bit(Requirement requirement) noexcept {
    return static_cast<std::uint64_t>(requirement);
}

struct TimingBudget {
    std::uint64_t deadline_ns{};
    std::uint64_t wcet_ns{};
    std::uint64_t blocking_ns{};
    std::uint64_t interference_ns{};
    std::uint64_t jitter_ns{};

    bool response_within_deadline() const noexcept {
        std::uint64_t total = 0;
        const auto add = [&total](std::uint64_t value) noexcept {
            if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
            total += value;
            return true;
        };
        return deadline_ns != 0
            && add(wcet_ns)
            && add(blocking_ns)
            && add(interference_ns)
            && add(jitter_ns)
            && total <= deadline_ns;
    }
};

struct Capabilities {
    bool full_guest_aot{};
    bool runtime_codegen_disabled{};
    bool dynamic_loading_disabled{};
    bool zero_tracing_gc_critical_path{};
    bool deterministic_actor_regions{};
    bool memory_preallocated{};
    bool memory_prefaulted{};
    bool workers_prestarted{};
    bool bounded_dispatch{};
    bool bounded_priority_inversion{};
    bool critical_worker_isolation{};
    bool os_realtime_policy{};
    bool bounded_cleanup{};
    bool bounded_call_graph{};
    bool bounded_loops{};
    bool bounded_queues{};
    bool blocking_io_absent{};
    bool ffi_qualified{};
    bool untrusted_jit_excluded{};
    bool timing_evidence{};
};

struct Admission {
    std::uint64_t missing{};
    bool timing_budget_ok{};

    bool admitted() const noexcept {
        return missing == 0 && timing_budget_ok;
    }

    bool lacks(Requirement requirement) const noexcept {
        return (missing & bit(requirement)) != 0;
    }
};

inline Admission evaluate_hard_realtime(
        const TimingBudget& timing,
        const Capabilities& capabilities) noexcept {
    std::uint64_t missing = 0;
    const auto require = [&missing](bool present, Requirement requirement) noexcept {
        if (!present) missing |= bit(requirement);
    };

    require(capabilities.full_guest_aot, Requirement::FullGuestAot);
    require(capabilities.runtime_codegen_disabled, Requirement::RuntimeCodegenDisabled);
    require(capabilities.dynamic_loading_disabled, Requirement::DynamicLoadingDisabled);
    require(capabilities.zero_tracing_gc_critical_path, Requirement::ZeroTracingGcCriticalPath);
    require(capabilities.deterministic_actor_regions, Requirement::DeterministicActorRegions);
    require(capabilities.memory_preallocated, Requirement::MemoryPreallocated);
    require(capabilities.memory_prefaulted, Requirement::MemoryPrefaulted);
    require(capabilities.workers_prestarted, Requirement::WorkersPrestarted);
    require(capabilities.bounded_dispatch, Requirement::BoundedDispatch);
    require(capabilities.bounded_priority_inversion, Requirement::BoundedPriorityInversion);
    require(capabilities.critical_worker_isolation, Requirement::CriticalWorkerIsolation);
    require(capabilities.os_realtime_policy, Requirement::OsRealtimePolicy);
    require(capabilities.bounded_cleanup, Requirement::BoundedCleanup);
    require(capabilities.bounded_call_graph, Requirement::BoundedCallGraph);
    require(capabilities.bounded_loops, Requirement::BoundedLoops);
    require(capabilities.bounded_queues, Requirement::BoundedQueues);
    require(capabilities.blocking_io_absent, Requirement::BlockingIoAbsent);
    require(capabilities.ffi_qualified, Requirement::FfiQualified);
    require(capabilities.untrusted_jit_excluded, Requirement::UntrustedJitExcluded);
    require(capabilities.timing_evidence, Requirement::TimingEvidence);

    return Admission{missing, timing.response_within_deadline()};
}

} // namespace oreslang::runtime::realtime
