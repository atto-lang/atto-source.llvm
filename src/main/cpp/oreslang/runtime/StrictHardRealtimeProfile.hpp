#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime::realtime {

// Independent strict target/profile gate. No call may claim certification
// merely from this host-level value check: source MIR and linked ELF proofs,
// WCET analysis and measured hardware/RTOS evidence remain mandatory.
enum class StrictRtDenial : std::uint8_t {
    none, unqualified_target, guest_compilation_missing,
    collector_or_dynamic_code, unbounded_effects, scheduler_not_qualified,
    memory_not_prepared, resources_unbounded, timing_unproven
};

struct StrictRtProfile final {
    bool named_os_hardware_qualified = false;
    bool compiled_guest_full_aot = false;
    bool linked_image_zero_tracing_gc = false;
    bool runtime_codegen_and_loading_absent = false;
    bool closed_call_graph_and_ffi = false;
    bool bounded_loops_and_cleanup = false;
    bool no_blocking_io_on_carrier = false;
    bool scheduler_and_preemption_qualified = false;
    bool physical_interference_bounded = false;
    bool memory_preallocated_and_prefaulted = false;
    bool actor_arenas_and_mailboxes_fixed = false;
    bool compiler_effects_verified = false;
    bool independent_wcet_evidence = false;
    std::size_t actor_arena_bytes = 0;
    std::size_t data_mailbox_slots = 0;
    std::size_t reserved_control_slots = 0;
    std::uint64_t deadline_ns = 0;
    std::uint64_t period_ns = 0;
    std::uint64_t wcet_ns = 0;
    std::uint64_t blocking_ns = 0;
    std::uint64_t interference_ns = 0;
    std::uint64_t jitter_ns = 0;
};

constexpr StrictRtDenial admit_strict_hard_rt(
        const StrictRtProfile& p) noexcept {
    if (!p.named_os_hardware_qualified)
        return StrictRtDenial::unqualified_target;
    if (!p.compiled_guest_full_aot || !p.compiler_effects_verified)
        return StrictRtDenial::guest_compilation_missing;
    if (!p.linked_image_zero_tracing_gc ||
        !p.runtime_codegen_and_loading_absent)
        return StrictRtDenial::collector_or_dynamic_code;
    if (!p.closed_call_graph_and_ffi || !p.bounded_loops_and_cleanup ||
        !p.no_blocking_io_on_carrier)
        return StrictRtDenial::unbounded_effects;
    if (!p.scheduler_and_preemption_qualified ||
        !p.physical_interference_bounded)
        return StrictRtDenial::scheduler_not_qualified;
    if (!p.memory_preallocated_and_prefaulted)
        return StrictRtDenial::memory_not_prepared;
    if (!p.actor_arenas_and_mailboxes_fixed || p.actor_arena_bytes == 0 ||
        p.data_mailbox_slots == 0 || p.reserved_control_slots == 0)
        return StrictRtDenial::resources_unbounded;
    if (!p.independent_wcet_evidence || p.wcet_ns == 0 ||
        p.deadline_ns == 0 || p.period_ns < p.deadline_ns)
        return StrictRtDenial::timing_unproven;
    // Subtraction is overflow-safe even for hostile UINT64_MAX inputs.
    std::uint64_t remaining = p.deadline_ns;
    if (p.wcet_ns > remaining) return StrictRtDenial::timing_unproven;
    remaining -= p.wcet_ns;
    if (p.blocking_ns > remaining) return StrictRtDenial::timing_unproven;
    remaining -= p.blocking_ns;
    if (p.interference_ns > remaining) return StrictRtDenial::timing_unproven;
    remaining -= p.interference_ns;
    if (p.jitter_ns > remaining) return StrictRtDenial::timing_unproven;
    return StrictRtDenial::none;
}

} // namespace oreslang::runtime::realtime
