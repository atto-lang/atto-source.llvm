#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using oreslang::runtime::NativeThreadJoinReaperV1;
using oreslang::runtime::NativeJoinCompletionV1;

static std::atomic<bool> gate{false};
static std::atomic<int> cleanup{0};
struct Arg { int value; };
static int entry(void* owned) {
    Arg* arg = static_cast<Arg*>(owned);
    while (!gate.load(std::memory_order_acquire))
        std::this_thread::yield(); // host test only; not a guest carrier
    return arg->value;
}
static void drop(void* owned) {
    delete static_cast<Arg*>(owned);
    cleanup.fetch_add(1);
}
static void read_when_ready(std::shared_ptr<NativeJoinCompletionV1> p, int value) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int out = -1;
    while (!p->try_read(status, out)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(status == ORES_THREAD_OK_V1);
    assert(out == value);
}
int main() {
    try { NativeThreadJoinReaperV1 bad(0); assert(false); }
    catch (const std::invalid_argument&) {}
    NativeThreadJoinReaperV1 reaper(2);
    ores_native_thread_v1* handles[3]{};
    for (int i = 0; i < 3; ++i)
        assert(ores_native_thread_spawn_v1(entry, new Arg{42+i}, drop, &handles[i])
               == ORES_THREAD_OK_V1);

    auto first = reaper.submit(handles[0]);
    auto discarded_waiter = reaper.submit(handles[1]);
    assert(first && discarded_waiter && !handles[0] && !handles[1]);
    // No thread has exited, so this confirms enqueue never waits for join.
    ores_thread_status_v1 provisional = ORES_THREAD_OS_ERROR_V1;
    int provisional_value = -1;
    assert(!first->try_read(provisional, provisional_value));
    assert(reaper.active() == 2);
    assert(!reaper.submit(handles[2])); // bounded backpressure
    assert(handles[2] != nullptr);      // caller still owns rejected submission
    discarded_waiter.reset();          // dropped observer cannot cancel thread cleanup

    gate.store(true, std::memory_order_release);
    read_when_ready(first, 42);
    int result = -1;
    assert(ores_native_thread_join_blocking_v1(handles[2], &result) == ORES_THREAD_OK_V1);
    assert(result == 44);
    assert(ores_native_thread_release_v1(handles[2]) == ORES_THREAD_OK_V1);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (cleanup.load() != 3 || reaper.active() != 0) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    std::cout << "native bounded async-join reaper: OK (host only)\n";
}
