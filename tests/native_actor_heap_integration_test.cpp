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
    std::atomic<OwnedHeap*> heap{nullptr};
};
struct Payload {
    explicit Payload(std::atomic<int>& drops) noexcept : drops_(&drops) {}
    ~Payload() noexcept { drops_->fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<int>* drops_;
};

static ActorStep handler(void* actor_state, ActorMessage message,
                         bool resumed, std::int64_t result) noexcept {
    auto& state = *static_cast<State*>(actor_state);
    auto* heap = HungryCarrier::current_actor_heap();
    if (!heap) {
        ++state.failures;
        return {StepKind::done, 0, 0};
    }
    auto* expected = static_cast<OwnedHeap*>(nullptr);
    if (!state.heap.compare_exchange_strong(expected, heap) &&
        state.heap.load() != heap) {
        ++state.failures;
    }
    if (message.id == 99 && !resumed)
        return {StepKind::await_io, 5, 0};
    if (message.id == 99 && result != 10) ++state.failures;

    try {
        auto owned = heap->make<Payload>(state.destroyed);
        auto borrow = owned.borrow();
        borrow.with([](const Payload&) {});
        borrow.reset();
        owned.drop();
    } catch (...) {
        ++state.failures;
    }
    ++state.finished;
    return {StepKind::done, 0, 0};
}

static std::int64_t broker(void* broker_state, std::int64_t value,
                           const std::atomic<bool>&) noexcept {
    auto& state = *static_cast<State*>(broker_state);
    if (HungryCarrier::current_actor_heap() != nullptr) ++state.failures;
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
    check(HungryCarrier::current_actor_heap() == nullptr,
          "supervisor must not access actor-local heap");
    State a, b;
    HungryCarrier::Options opts;
    opts.actor_heap_limit_bytes = sizeof(Payload);
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
          "actor-owned objects not dropped exactly once");
    check(a.failures == 0 && b.failures == 0,
          "broker/handler crossed actor memory boundary");
    check(a.broker_calls == 1 && b.broker_calls == 0,
          "broker async contract was not exercised");
    check(a_stats.actor_heap_closed && b_stats.actor_heap_closed,
          "actor heap retirement did not close");
    check(a_stats.actor_live_objects == 0 && b_stats.actor_live_objects == 0 &&
          a_stats.actor_live_bytes == 0 && b_stats.actor_live_bytes == 0,
          "actor-owned heap retained allocations");
    check(a_stats.protocol_failures == 0 && b_stats.protocol_failures == 0,
          "carrier reported lifecycle failure");
    check(HungryCarrier::current_actor_heap() == nullptr,
          "actor-local heap leaked into supervisor thread");
    std::cout << "PASS: native carriers have separate ownership heaps; no heap "
                 "access on broker/supervisor, async resume, deterministic "
                 "drop and retirement\n";
}
