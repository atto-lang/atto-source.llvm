#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <pthread.h>
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
static ores_thread_status_v1 always_fail(ores_native_thread_v1*, int*) {
    return ORES_THREAD_OS_ERROR_V1;
}
static void test_failed_handles_keep_capacity_until_recovery() {
    const int baseline = cleanup.load();
    NativeThreadJoinReaperV1 reaper(2, 2, always_fail);
    ores_native_thread_v1* handles[3] = {
        spawn(610), spawn(611), spawn(612)
    };
    auto completions[2] = {
        reaper.submit(handles[0]), reaper.submit(handles[1])
    };
    assert(completions[0] && completions[1]);
    assert(!handles[0] && !handles[1]);
    wait_read(completions[0], ORES_THREAD_OS_ERROR_V1);
    wait_read(completions[1], ORES_THREAD_OS_ERROR_V1);
    assert(reaper.failed_count() == 2);
    assert(reaper.active() == 2); // failure still owns both bounded slots
    assert(!reaper.submit(handles[2]));
    assert(handles[2] != nullptr); // rejected submission retains caller ownership

    auto failed = reaper.recover_failed();
    assert(failed.size() == 2);
    assert(reaper.failed_count() == 0);
    assert(reaper.active() == 0);
    for (std::size_t i = 0; i < failed.size(); ++i) {
        int result = -1;
        assert(ores_native_thread_join_blocking_v1(failed[i], &result)
               == ORES_THREAD_OK_V1);
        assert(result == 610 + static_cast<int>(i));
        assert(ores_native_thread_release_v1(failed[i]) == ORES_THREAD_OK_V1);
    }
    int result = -1;
    assert(ores_native_thread_join_blocking_v1(handles[2], &result)
           == ORES_THREAD_OK_V1);
    assert(result == 612);
    assert(ores_native_thread_release_v1(handles[2]) == ORES_THREAD_OK_V1);
    wait_cleanup(baseline + 3, reaper);
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
static int early_exit_entry(void* owned) {
    (void)owned;
    pthread_exit(nullptr);
    return 0;
}
static void test_abnormal_exit_consumes_join() {
    NativeThreadJoinReaperV1 reaper(1, 1);
    auto* arg = new Arg{55, nullptr};
    ores_native_thread_v1* handle = nullptr;
    assert(ores_native_thread_spawn_v1(early_exit_entry, arg, drop, &handle)
           == ORES_THREAD_OK_V1);
    auto completion = reaper.submit(handle);
    assert(completion && !handle);
    wait_read(completion, ORES_THREAD_UNEXPECTED_EXIT_V1);
    wait_cleanup(7, reaper);
    // The physical join succeeded. There must not be a spurious failed-join
    // handle to retry or a double release hidden in the error path.
    assert(reaper.failed_count() == 0);
}
static void test_nonblocking_completion_subscription() {
    // Race before completion and after completion. A Task integration can
    // install a weak-generation-checked wake callback with no busy polling.
    const int before = cleanup.load();
    gate_a.store(false, std::memory_order_release);
    NativeThreadJoinReaperV1 reaper(2, 2);
    auto* pending = spawn(77, &gate_a);
    auto subscribed = reaper.submit(pending);
    assert(subscribed && !pending);
    std::atomic<int> notifications{0};
    assert(subscribed->on_ready_once([&] {
        ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
        int result = 0;
        assert(subscribed->try_read(status, result));
        assert(status == ORES_THREAD_OK_V1 && result == 77);
        notifications.fetch_add(1);
    }));
    assert(!subscribed->on_ready_once([&] { notifications.fetch_add(100); }));
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int result = 0;
    assert(!subscribed->try_read(status, result));

    // A cancelled task waiter is represented by a weak handle. Its disposal
    // does not drop or cancel the native pthread/JoinHandle.
    gate_b.store(false, std::memory_order_release);
    auto* canceled = spawn(78, &gate_b);
    auto abandoned = reaper.submit(canceled);
    assert(abandoned && !canceled);
    auto live = std::make_shared<int>(1);
    std::weak_ptr<int> weak = live;
    std::atomic<int> stale_wakes{0};
    assert(abandoned->on_ready_once([weak, &stale_wakes] {
        if (weak.lock()) stale_wakes.fetch_add(1);
    }));
    live.reset();
    gate_a.store(true, std::memory_order_release);
    gate_b.store(true, std::memory_order_release);
    wait_read(subscribed, ORES_THREAD_OK_V1, 77);
    wait_read(abandoned, ORES_THREAD_OK_V1, 78);
    // Callback runs after ready is published, but may run slightly later
    // than a polling observer; synchronization is needed for its own count.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    while (notifications.load() != 1) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(stale_wakes.load() == 0);
    assert(!subscribed->on_ready_once([] {}));

    // Registration after physical completion must immediately notify once.
    std::atomic<int> late{0};
    assert(abandoned->on_ready_once([&] { late.fetch_add(1); }) == false);
    auto* fast = spawn(79);
    auto done = reaper.submit(fast);
    assert(done && !fast);
    wait_read(done, ORES_THREAD_OK_V1, 79);
    assert(done->on_ready_once([&] { late.fetch_add(1); }));
    assert(late.load() == 1);
    assert(!done->on_ready_once([&] { late.fetch_add(1); }));
    wait_cleanup(before + 3, reaper);
}

static void test_notification_registration_race() {
    const int baseline = cleanup.load();
    NativeThreadJoinReaperV1 reaper(4, 2);
    std::atomic<int> wakes{0};
    for (int i = 0; i < 24; ++i) {
        auto* handle = spawn(300 + i);
        auto completion = reaper.submit(handle);
        assert(completion && !handle);
        std::thread registrar([completion, &wakes] {
            assert(completion->on_ready_once([&wakes] {
                wakes.fetch_add(1, std::memory_order_relaxed);
            }));
        });
        wait_read(completion, ORES_THREAD_OK_V1, 300 + i);
        registrar.join();
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    while (wakes.load() != 24) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    wait_cleanup(baseline + 24, reaper);
}
int main() {
    test_invalid_limits();
    test_no_head_of_line_blocking();
    test_failed_join_recovery();
    test_failed_handles_keep_capacity_until_recovery();
    test_abnormal_exit_consumes_join();
    test_nonblocking_completion_subscription();
    test_notification_registration_race();
    std::cout << "native concurrent join reaper: OK (host-only)\n";
}
