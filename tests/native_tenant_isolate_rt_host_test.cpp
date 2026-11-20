#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include "oreslang/runtime/StrictHardRealtimeProfile.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <atomic>

using oreslang::runtime::NativeTenantAdmission;
using namespace oreslang::runtime::realtime;

int main() {
    NativeTenantAdmission vm(2, 3, 32);
    auto a = vm.open(); auto b = vm.open();
    assert(a && b && a->slot != b->slot && !vm.open());
    auto a0 = vm.enter(*a, 0); auto b0 = vm.enter(*b, 0);
    assert(a0 && b0 && !vm.enter(*a, 0) && !vm.enter(*a, 3));
    std::uint64_t number = 91, out = 0;
    assert(a0->write(0, &number, sizeof(number)));
    assert(b0->read(0, &out, sizeof(out)) && out == 0);
    assert(!a0->write(31, &number, sizeof(number)));
    assert(!a0->write(std::numeric_limits<std::size_t>::max(), &number, 1));
    assert(!vm.request_stop({a->slot, a->generation + 1}));
    assert(vm.request_stop(*a));
    assert(!vm.enter(*a, 1) && !vm.finish_stop(*a));
    assert(a0->read(0, &out, sizeof(out)) && out == number);
    a0.reset();
    assert(vm.finish_stop(*a) && !vm.finish_stop(*a));
    auto c = vm.open();
    assert(c && c->slot == a->slot && c->generation > a->generation);
    assert(!vm.enter(*a, 0));
    auto c0 = vm.enter(*c, 0);
    assert(c0 && c0->read(0, &out, sizeof(out)) && out == 0);
    auto c1 = vm.enter(*c, 1);
    assert(c1);
    std::uint64_t second = 77;
    assert(c1->write(0, &second, sizeof(second)));
    assert(c0->read(0, &out, sizeof(out)) && out == 0);
    c0.reset(); c1.reset();
    assert(vm.request_stop(*c) && vm.finish_stop(*c));
    b0.reset();
    assert(vm.request_stop(*b) && vm.finish_stop(*b));

    // The retained state backing must outlive the registry while a turn holds
    // its RAII release pin, avoiding a registry-destructor use-after-free.
    std::optional<NativeTenantAdmission::Turn> late;
    {
        NativeTenantAdmission ephemeral(1, 1, 16);
        auto h = ephemeral.open(); assert(h);
        late = ephemeral.enter(*h, 0);
        assert(late);
    }
    late.reset();

    // Independent actors may run simultaneously in one trusted tenant.
    NativeTenantAdmission parallel(1, 2, 16);
    auto h = parallel.open(); assert(h);
    std::atomic<int> ready{0};
    auto worker = [&](std::size_t index) {
        auto turn = parallel.enter(*h, index);
        assert(turn);
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
        const std::uint64_t v = static_cast<std::uint64_t>(index + 1);
        std::uint64_t got = 0;
        assert(turn->write(0, &v, sizeof(v)));
        assert(turn->read(0, &got, sizeof(got)) && got == v);
    };
    std::thread t0(worker, 0), t1(worker, 1);
    t0.join(); t1.join();
    assert(parallel.request_stop(*h) && parallel.finish_stop(*h));

    // No positive profile claims absent independently earned predicates.
    StrictRtProfile p;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::unqualified_target);
    p.named_os_hardware_qualified = true;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::guest_compilation_missing);
    p.compiled_guest_full_aot = p.compiler_effects_verified = true;
    p.linked_image_zero_tracing_gc = p.runtime_codegen_and_loading_absent = true;
    p.closed_call_graph_and_ffi = p.bounded_loops_and_cleanup = true;
    p.no_blocking_io_on_carrier = true;
    p.scheduler_and_preemption_qualified = p.physical_interference_bounded = true;
    p.memory_preallocated_and_prefaulted = true;
    p.actor_arenas_and_mailboxes_fixed = true;
    p.actor_arena_bytes = 4096; p.data_mailbox_slots = 8;
    p.reserved_control_slots = 4;
    p.independent_wcet_evidence = true;
    p.deadline_ns = 1000; p.period_ns = 1000; p.wcet_ns = 500;
    p.blocking_ns = 100; p.interference_ns = 100; p.jitter_ns = 100;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::none);
    p.jitter_ns = std::numeric_limits<std::uint64_t>::max();
    assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven);
    p.jitter_ns = 100; p.wcet_ns = 0;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven);
    p.wcet_ns = 500; p.period_ns = 999;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::timing_unproven);
    p.period_ns = 1000; p.reserved_control_slots = 0;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::resources_unbounded);
    p.reserved_control_slots = 4; p.linked_image_zero_tracing_gc = false;
    assert(admit_strict_hard_rt(p) == StrictRtDenial::collector_or_dynamic_code);
}
