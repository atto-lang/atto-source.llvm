#include "oreslang/runtime/HardRealtimeContract.hpp"

#include <cassert>

using namespace oreslang::runtime::rt;

int main() {
    TimingBudget good{};
    good.deadline_ns = 1'000'000;
    good.wcet_ns = 500'000;
    good.max_blocking_ns = 100'000;
    good.max_jitter_ns = 50'000;
    good.period_ns = 10'000'000;
    good.max_memory_bytes = 64 * 1024;

    assert(admit_native_hard_rt({}, good));

    Effects managed{};
    managed.managed_heap = true;
    assert(admit_native_hard_rt(managed, good).failure ==
           AdmissionFailure::ManagedHeap);

    Effects ffi{};
    ffi.unknown_ffi = true;
    assert(admit_native_hard_rt(ffi, good).failure ==
           AdmissionFailure::UnknownFfi);

    Effects cleanup{};
    cleanup.unbounded_cleanup = true;
    assert(admit_native_hard_rt(cleanup, good).failure ==
           AdmissionFailure::UnboundedCleanup);

    TimingBudget impossible = good;
    impossible.wcet_ns = 950'000;
    impossible.max_blocking_ns = 100'000;
    assert(!admit_native_hard_rt({}, impossible));

    TimingBudget missing{};
    assert(admit_native_hard_rt({}, missing).failure ==
           AdmissionFailure::MissingTimingBudget);

    static_assert(diagnostic_code(AdmissionFailure::UnboundedLoop) ==
                  "RT008_UNBOUNDED_LOOP");

    return 0;
}
