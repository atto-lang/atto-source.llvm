#include "oreslang/runtime/StrictHardRealtimeProfile.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
using namespace oreslang::runtime::realtime;

static StrictRtProfile good_fixture() {
    StrictRtProfile p;
    p.named_os_hardware_qualified = true;
    p.compiled_guest_full_aot = p.compiler_effects_verified = true;
    p.linked_image_zero_tracing_gc = p.runtime_codegen_and_loading_absent = true;
    p.closed_call_graph_and_ffi = p.bounded_loops_and_cleanup = true;
    p.no_blocking_io_on_carrier = true;
    p.scheduler_and_preemption_qualified = p.physical_interference_bounded = true;
    p.memory_preallocated_and_prefaulted = p.actor_arenas_and_mailboxes_fixed = true;
    p.independent_wcet_evidence = true;
    p.actor_arena_bytes = 4096;
    p.data_mailbox_slots = 8;
    p.reserved_control_slots = 4;
    p.deadline_ns = 1000;
    p.period_ns = 1000;
    p.wcet_ns = 500;
    p.blocking_ns = p.interference_ns = p.jitter_ns = 100;
    return p;
}

int main() {
    const auto good = good_fixture();
    // Synthetic all-true input only tests predicate arithmetic; it is not
    // authenticated source/MIR/ELF/WCET evidence or an RT qualification.
    assert(admit_strict_hard_rt(good) == StrictRtDenial::none);
#define DENY_FALSE(field, expected) { auto p = good; p.field = false; assert(admit_strict_hard_rt(p) == StrictRtDenial::expected); }
    DENY_FALSE(named_os_hardware_qualified, unqualified_target);
    DENY_FALSE(compiled_guest_full_aot, guest_compilation_missing);
    DENY_FALSE(compiler_effects_verified, guest_compilation_missing);
    DENY_FALSE(linked_image_zero_tracing_gc, collector_or_dynamic_code);
    DENY_FALSE(runtime_codegen_and_loading_absent, collector_or_dynamic_code);
    DENY_FALSE(closed_call_graph_and_ffi, unbounded_effects);
    DENY_FALSE(bounded_loops_and_cleanup, unbounded_effects);
    DENY_FALSE(no_blocking_io_on_carrier, unbounded_effects);
    DENY_FALSE(scheduler_and_preemption_qualified, scheduler_not_qualified);
    DENY_FALSE(physical_interference_bounded, scheduler_not_qualified);
    DENY_FALSE(memory_preallocated_and_prefaulted, memory_not_prepared);
    DENY_FALSE(actor_arenas_and_mailboxes_fixed, resources_unbounded);
    DENY_FALSE(independent_wcet_evidence, timing_unproven);
#undef DENY_FALSE
    { auto p = good; p.actor_arena_bytes = 0; assert(admit_strict_hard_rt(p) == StrictRtDenial::resources_unbounded); }
    { auto p = good; p.data_mailbox_slots = 0; assert(admit_strict_hard_rt(p) == StrictRtDenial::resources_unbounded); }
    { auto p = good; p.reserved_control_slots = 0; assert(admit_strict_hard_rt(p) == StrictRtDenial::resources_unbounded); }
    { auto p = good; p.wcet_ns = 0; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.deadline_ns = 0; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.period_ns = 999; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.wcet_ns = 1001; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.wcet_ns = 801; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.blocking_ns = 301; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.interference_ns = 301; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.jitter_ns = 301; assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.wcet_ns = std::numeric_limits<std::uint64_t>::max(); assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.blocking_ns = std::numeric_limits<std::uint64_t>::max(); assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.interference_ns = std::numeric_limits<std::uint64_t>::max(); assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.jitter_ns = std::numeric_limits<std::uint64_t>::max(); assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven); }
    { auto p = good; p.wcet_ns = 700; assert(admit_strict_hard_rt(p) == StrictRtDenial::none); }
}
