#include "oreslang/runtime/NativeTraceProbePolicy.hpp"

#include <cassert>
#include <thread>
#include <atomic>
#include <vector>

using oreslang::runtime::NativeTraceProbePolicy;
using oreslang::runtime::TraceLevel;

// HOST-ONLY native policy exercise; not an LLVM-compiled Atto guest, nor a
// secure context-propagation or OTLP exporter validation.
int main() {
    NativeTraceProbePolicy only_boundaries(TraceLevel::Boundaries);
    assert(!only_boundaries.should_record(TraceLevel::Boundaries));
    assert(only_boundaries.generation() == 0);
    assert(only_boundaries.reconfigure(TraceLevel::Boundaries));
    assert(only_boundaries.should_record(TraceLevel::Boundaries));
    assert(!only_boundaries.should_record(TraceLevel::Calls));
    assert(only_boundaries.generation() == 1);
    assert(!only_boundaries.reconfigure(TraceLevel::Calls));
    assert(only_boundaries.generation() == 1);
    assert(only_boundaries.reconfigure(TraceLevel::Off));
    assert(!only_boundaries.should_record(TraceLevel::Boundaries));

    NativeTraceProbePolicy stripped(TraceLevel::Off);
    assert(!stripped.reconfigure(TraceLevel::Boundaries));
    assert(!stripped.should_record(TraceLevel::Boundaries));

    NativeTraceProbePolicy hard_rt(TraceLevel::Full, true);
    assert(!hard_rt.reconfigure(TraceLevel::Full));
    assert(!hard_rt.should_record(TraceLevel::Full));

    NativeTraceProbePolicy first(TraceLevel::Calls);
    NativeTraceProbePolicy second(TraceLevel::Calls);
    assert(first.reconfigure(TraceLevel::Calls));
    assert(first.should_record(TraceLevel::Calls));
    assert(!second.should_record(TraceLevel::Calls));

    // An atomic mode and generation share one coherent publication; no torn
    // mode across simultaneous trusted policy writes and observer reads.
    NativeTraceProbePolicy concurrent(TraceLevel::Full);
    std::atomic<bool> done{false};
    std::thread reader([&]() {
        while (!done.load(std::memory_order_acquire)) {
            const auto level = concurrent.mode();
            assert(level == TraceLevel::Off || level == TraceLevel::Boundaries
                || level == TraceLevel::Calls || level == TraceLevel::Full);
            (void)concurrent.should_record(TraceLevel::Calls);
        }
    });
    for (int i = 0; i < 10000; ++i) {
        assert(concurrent.reconfigure(i % 2 ? TraceLevel::Off : TraceLevel::Full));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    assert(concurrent.generation() <= 10000);
    return 0;
}
