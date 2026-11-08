#include "oreslang/runtime/HungryCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void require(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}

struct ActorState {
    std::atomic<int> processed{0};
    std::atomic<int> failures{0};
    std::atomic<bool> exhausted{false};
    std::atomic<HungryCarrier::NativeStack*> seen_stack{nullptr};
    HungryCarrier::NativeStack::FrameRef paused{};
};

static ActorStep on_message(void* opaque, ActorMessage message,
                            bool resumed, std::int64_t io_result) noexcept {
    auto& state = *static_cast<ActorState*>(opaque);
    auto* stack = HungryCarrier::active_stack();
    auto* heap = HungryCarrier::active_heap();
    if (!stack || !heap) {
        ++state.failures;
        return {};
    }
    auto* existing = static_cast<HungryCarrier::NativeStack*>(nullptr);
    if (!state.seen_stack.compare_exchange_strong(existing, stack) &&
        state.seen_stack.load() != stack)
        ++state.failures;

    if (message.id == 500) {
        // Deliberately exceed 64 bounded logical frames, never recurse in C++.
        for (unsigned i = 0; i != 65; ++i) {
            if (!stack->push(8)) {
                state.exhausted.store(stack->poisoned(), std::memory_order_release);
                break;
            }
        }
        return {}; // runtime must STOP even if callback swallows the failure
    }
    if (message.id == 99) {
        if (!resumed) {
            auto frame = stack->push(sizeof(std::int64_t));
            const std::int64_t marker = 4711;
            if (!frame || !stack->write(*frame, 0, &marker, sizeof marker)) {
                ++state.failures;
                return {};
            }
            state.paused = *frame;
            return {StepKind::await_io, 5, 1};
        }
        std::int64_t marker = 0;
        if (io_result != 10 ||
            !stack->read(state.paused, 0, &marker, sizeof marker) ||
            marker != 4711 || !stack->pop(state.paused))
            ++state.failures;
        ++state.processed;
        return {};
    }

    auto frame = stack->push(sizeof(std::int64_t));
    auto input = message.input;
    std::int64_t output = 0;
    if (!frame || !stack->write(*frame, 0, &input, sizeof input) ||
        !stack->read(*frame, 0, &output, sizeof output) ||
        output != input || !stack->pop(*frame))
        ++state.failures;
    ++state.processed;
    return {};
}

static std::int64_t io_broker(void* opaque, std::int64_t payload,
                              const std::atomic<bool>&) noexcept {
    auto& state = *static_cast<ActorState*>(opaque);
    if (HungryCarrier::active_stack() || HungryCarrier::active_heap())
        ++state.failures;
    return payload * 2;
}

template<typename Test>
static bool eventually(Test predicate) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
    return predicate();
}

int main() {
    try {
        require(HungryCarrier::active_stack() == nullptr,
                "supervisor received actor-local stack capability");
        ActorState good, failing;
        HungryCarrier::Options options;
        HungryCarrier sibling(&good, on_message, &good, io_broker, options);
        HungryCarrier exhausted(&failing, on_message, &failing, io_broker, options);
        sibling.start();
        exhausted.start();
        auto healthy_sender = sibling.producer_handle();
        auto broken_sender = exhausted.producer_handle();

        require(healthy_sender.try_send({99, 0}), "failed await admission");
        require(broken_sender.try_send({500, 0}), "failed budget test admission");
        require(healthy_sender.try_send({1, 21}) &&
                healthy_sender.try_send({2, 34}),
                "healthy sibling was unable to accept messages");

        require(eventually([&] { return good.processed.load() == 3; }),
                "healthy sibling did not resume its saved frame and finish");
        require(eventually([&] { return exhausted.stopped(); }),
                "frame exhaustion did not stop the affected actor");
        require(failing.exhausted.load(), "actor stack quota not enforced");
        require(good.failures.load() == 0 && failing.failures.load() == 0,
                "frame/heap authority leaked into another carrier or broker");

        sibling.join();
        exhausted.join();
        require(sibling.stats().protocol_failures == 0 &&
                exhausted.stats().protocol_failures > 0,
                "stack budget fault did not remain actor-local");
        auto* healthy_stack = good.seen_stack.load();
        auto* failed_stack = failing.seen_stack.load();
        require(healthy_stack && failed_stack && healthy_stack != failed_stack,
                "actors shared logical stack backing storage");
        require(healthy_stack->retired() && failed_stack->retired(),
                "actor STOP did not scrub and retire frame arenas");
        require(HungryCarrier::active_stack() == nullptr,
                "actor frame authority leaked back to supervisor");
        std::cout << "PASS actor carrier owned frame isolation, saved await "
                     "frame, terminal exhaustion, sibling survival and cleanup\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL native actor frame integration: " << e.what() << '\n';
        return 1;
    }
}
