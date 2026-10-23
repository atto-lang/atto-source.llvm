#include "oreslang/runtime/HardRealtimeAdmission.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace oreslang::runtime::realtime;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static Capabilities qualified() {
    return Capabilities{
        true, true, true, true, true,
        true, true, true, true, true,
        true, true, true, true, true,
        true, true, true, true, true
    };
}

int main() {
    try {
        TimingBudget good{1'000'000, 400'000, 100'000, 200'000, 50'000};
        auto admitted = evaluate_hard_realtime(good, qualified());
        require(admitted.admitted(), "qualified hard-RT profile was rejected");

        auto missing_memory = qualified();
        missing_memory.memory_prefaulted = false;
        auto memory_denial = evaluate_hard_realtime(good, missing_memory);
        require(!memory_denial.admitted(), "non-prefaulted memory was admitted");
        require(memory_denial.lacks(Requirement::MemoryPrefaulted),
                "prefault denial bit missing");

        auto lazy_workers = qualified();
        lazy_workers.workers_prestarted = false;
        auto worker_denial = evaluate_hard_realtime(good, lazy_workers);
        require(!worker_denial.admitted(), "lazy worker creation was admitted");
        require(worker_denial.lacks(Requirement::WorkersPrestarted),
                "worker prestart denial bit missing");

        auto jit = qualified();
        jit.runtime_codegen_disabled = false;
        jit.dynamic_loading_disabled = false;
        jit.untrusted_jit_excluded = false;
        auto jit_denial = evaluate_hard_realtime(good, jit);
        require(jit_denial.lacks(Requirement::RuntimeCodegenDisabled)
                    && jit_denial.lacks(Requirement::DynamicLoadingDisabled)
                    && jit_denial.lacks(Requirement::UntrustedJitExcluded),
                "runtime/JIT/dynamic code was not rejected");

        TimingBudget over{1'000, 700, 200, 150, 1};
        require(!evaluate_hard_realtime(over, qualified()).admitted(),
                "response-time budget above deadline was admitted");

        TimingBudget overflow{
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(), 1, 1, 1};
        require(!overflow.response_within_deadline(),
                "overflowed timing arithmetic failed open");

        Capabilities none{};
        auto fail_closed = evaluate_hard_realtime(good, none);
        require(!fail_closed.admitted(), "empty capability proof was admitted");
        require(fail_closed.missing != 0, "empty proof did not report missing requirements");

        std::cout << "PASS hard realtime admission is full-AOT, zero-tracing, prestarted, bounded and fail-closed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL hard realtime admission: " << failure.what() << "\n";
        return 1;
    }
}
