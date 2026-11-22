#include "oreslang/runtime/HungryCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct State {
    std::atomic<int> finished{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> failures{0};
    std::atomic<int> broker_calls{0};
    std::atomic<HungryCarrier::NativeHeap*> heap{nullptr};
};
static ActorStep handler(void* actor_state, ActorMessage message,
                         bool resumed, std::int64_t result) noexcept {
    auto& state = *static_cast<State*>(actor_state);
    auto* heap = HungryCarrier::active_heap();
    if (!heap) {
        ++state.failures;
        return {StepKind::done, 0, 0};
    }
    auto* expected = static_cast<HungryCarrier::NativeHeap*>(nullptr);
    if (!state.heap.compare_exchange_strong(expected, heap) &&
        state.heap.load() != heap) {
        ++state.failures;
    }
    if (message.id == 99 && !resumed)
        return {StepKind::await_io, 5, 0};
    if (message.id == 99 && result != 10) ++state.failures;

    auto owned = heap->allocate(sizeof(std::int64_t));
    std::int64_t observed = 0;
    if (!owned || !heap->write(*owned, 0, &message.input, sizeof message.input)
            || !heap->read(*owned, 0, &observed, sizeof observed)
            || observed != message.input || !heap->release(*owned)
            || heap->read(*owned, 0, &observed, sizeof observed)
            || heap->live_blocks() != 0) {
        ++state.failures;
    } else {
        ++state.destroyed; // exactly one reclaimed byte allocation per turn
    }
    ++state.finished;
    return {StepKind::done, 0, 0};
}

static std::int64_t broker(void* broker_state, std::int64_t value,
                           const std::atomic<bool>&) noexcept {
    auto& state = *static_cast<State*>(broker_state);
    if (HungryCarrier::active_heap() != nullptr) ++state.failures;
    ++state.broker_calls;
    return value * 2;
}

static bool until(const std::atomic<int>& value, int target) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (value.load() < target && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return value.load() >= target;
}

int main() {
    check(HungryCarrier::active_heap() == nullptr,
          "supervisor must not access actor-local heap");
    State a, b;
    HungryCarrier::Options opts;
    HungryCarrier first(&a, handler, &a, broker, opts);
    HungryCarrier second(&b, handler, &b, broker, opts);
    first.start();
    second.start();
    const auto send_first = first.producer_handle();
    const auto send_second = second.producer_handle();
    for (int i = 0; i < 20; ++i) {
        check(send_first.try_send({static_cast<std::uint64_t>(i + 1), i}),
              "first actor mailbox unexpectedly full");
    }
    check(send_first.try_send({99, 0}), "actor async message was rejected");
    for (int i = 0; i < 7; ++i) {
        check(send_second.try_send({static_cast<std::uint64_t>(i + 1), i}),
              "second actor mailbox unexpectedly full");
    }
    check(until(a.finished, 21) && until(b.finished, 7),
          "actor messages never completed");
    first.join();
    second.join();

    const auto a_stats = first.stats();
    const auto b_stats = second.stats();
    check(a.heap.load() != nullptr && b.heap.load() != nullptr &&
          a.heap.load() != b.heap.load(), "two native actors shared heap identity");
    check(a.destroyed == 21 && b.destroyed == 7,
          "actor-owned byte allocations not reclaimed exactly once");
    check(a.failures == 0 && b.failures == 0,
          "broker/handler crossed actor memory boundary");
    check(a.broker_calls == 1 && b.broker_calls == 0,
          "broker async contract was not exercised");
    check(a_stats.protocol_failures == 0 && b_stats.protocol_failures == 0,
          "carrier reported lifecycle failure");
    check(HungryCarrier::active_heap() == nullptr,
          "actor-local heap leaked into supervisor thread");
    std::cout << "PASS: native carriers have separate ownership heaps; no heap "
                 "access on broker/supervisor, async resume, deterministic "
                 "drop and retirement\n";
}
