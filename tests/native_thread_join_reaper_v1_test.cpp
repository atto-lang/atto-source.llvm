#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeThreadJoinReaperV1;
using oreslang::runtime::NativeJoinCompletionV1;

static std::atomic<bool> gate_a{false}, gate_b{false};
static std::atomic<int> cleanup{0};
static std::atomic<bool> fail_next_join{false};

struct Arg {
    int value;
    std::atomic<bool>* gate;
};
static int entry(void* owned) {
    const auto* arg = static_cast<Arg*>(owned);
    if (arg->gate) {
        while (!arg->gate->load(std::memory_order_acquire))
            std::this_thread::yield(); // host test only
    }
    return arg->value;
}
static void drop(void* owned) {
    delete static_cast<Arg*>(owned);
    cleanup.fetch_add(1);
}
static ores_native_thread_v1* spawn(int value, std::atomic<bool>* gate = nullptr) {
    auto* arg = new Arg{value, gate};
    ores_native_thread_v1* handle = nullptr;
    auto status = ores_native_thread_spawn_v1(entry, arg, drop, &handle);
    if (status != ORES_THREAD_OK_V1) {
        delete arg; // ownership not transferred
        throw std::runtime_error("native_spawn_failed");
    }
    return handle;
}
static bool wait_read(const std::shared_ptr<NativeJoinCompletionV1>& completion,
                      ores_thread_status_v1 expected_status, int expected_value = 0) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int value = -1;
    while (!completion->try_read(status, value)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(status == expected_status);
    if (status == ORES_THREAD_OK_V1) assert(value == expected_value);
    return true;
}
static void wait_cleanup(int n, NativeThreadJoinReaperV1& reaper) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    while (cleanup.load() != n || reaper.active() != 0) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static ores_thread_status_v1 fail_once(ores_native_thread_v1* handle, int* result) {
    if (fail_next_join.exchange(false)) return ORES_THREAD_OS_ERROR_V1;
    return ores_native_thread_join_blocking_v1(handle, result);
}
static void test_invalid_limits() {
    try { NativeThreadJoinReaperV1 invalid(0); assert(false); }
    catch (const std::invalid_argument&) {}
    try { NativeThreadJoinReaperV1 invalid(2, 0); assert(false); }
    catch (const std::invalid_argument&) {}
    try { NativeThreadJoinReaperV1 invalid(2, 3); assert(false); }
    catch (const std::invalid_argument&) {}
    try { NativeThreadJoinReaperV1 invalid(1, 1, nullptr); assert(false); }
    catch (const std::invalid_argument&) {}
}
static void test_no_head_of_line_blocking() {
    NativeThreadJoinReaperV1 reaper(2, 2);
    auto* slow = spawn(40, &gate_a);
    auto* other = spawn(41, &gate_b);
    auto* fast = spawn(42);

    auto first = reaper.submit(slow);
    auto second = reaper.submit(other);
    assert(first && second && slow == nullptr && other == nullptr);
    // Saturation transfers no ownership from the rejected caller.
    assert(!reaper.submit(fast));
    assert(fast != nullptr);

    // The second host reaper must collect its independent worker, although
    // the first worker remains blocked in user code.
    gate_b.store(true, std::memory_order_release);
    wait_read(second, ORES_THREAD_OK_V1, 41);
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int value = -1;
    assert(!first->try_read(status, value));

    auto third = reaper.submit(fast);
    assert(third && fast == nullptr);
    wait_read(third, ORES_THREAD_OK_V1, 42);
    assert(!first->try_read(status, value));

    gate_a.store(true, std::memory_order_release);
    wait_read(first, ORES_THREAD_OK_V1, 40);
    wait_cleanup(3, reaper);

    // A dropped observer is not permitted to cancel native cleanup.
    auto* abandoned = spawn(43);
    auto dropped = reaper.submit(abandoned);
    assert(dropped && !abandoned);
    dropped.reset();
    wait_cleanup(4, reaper);
    assert(reaper.failed_count() == 0);
}
static void test_failed_join_recovery() {
    NativeThreadJoinReaperV1 reaper(1, 1, fail_once);
    for (int i = 0; i < 2; ++i) {
        auto* handle = spawn(50 + i);
        fail_next_join.store(true);
        auto completion = reaper.submit(handle);
        assert(completion && !handle);
        wait_read(completion, ORES_THREAD_OS_ERROR_V1);
        assert(reaper.failed_count() == 1);
        auto failed = reaper.recover_failed();
        assert(failed.size() == 1);
        assert(reaper.failed_count() == 0);
        int result = -1;
        assert(ores_native_thread_join_blocking_v1(failed.front(), &result)
               == ORES_THREAD_OK_V1);
        assert(result == 50 + i);
        assert(ores_native_thread_release_v1(failed.front()) == ORES_THREAD_OK_V1);
        wait_cleanup(5 + i, reaper);
    }
}
int main() {
    test_invalid_limits();
    test_no_head_of_line_blocking();
    test_failed_join_recovery();
    std::cout << "native concurrent join reaper: OK (host-only)\n";
}
