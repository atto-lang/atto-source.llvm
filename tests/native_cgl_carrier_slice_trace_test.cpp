#include "oreslang/runtime/CglCarrierDispatchTrace.hpp"
#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace oreslang::runtime;

struct ActorState {
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> canceled{0};
};
static MultiActorCarrier::Outcome finish_message(
        void* raw, MultiActorCarrier::Message msg,
        MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& state = *static_cast<ActorState*>(raw);
    if (cause != MultiActorCarrier::Cause::initial || fuel == 0 ||
        msg.sequence != state.completed.load(std::memory_order_relaxed) + 1) {
        return static_cast<MultiActorCarrier::Outcome>(255);
    }
    state.completed.fetch_add(1, std::memory_order_release);
    return MultiActorCarrier::Outcome::complete;
}
static void cleanup(void* raw, MultiActorCarrier::Message, bool) noexcept {
    static_cast<ActorState*>(raw)->canceled.fetch_add(1, std::memory_order_release);
}
static void until_complete(const ActorState& state, std::uint64_t desired) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (state.completed.load(std::memory_order_acquire) < desired) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
    }
}
int main() {
    // An empty or malformed trace cannot be represented as complete coverage.
    CglCarrierDispatchTrace empty;
    assert(!empty.complete_after_join());
    assert(!empty.matches_expected_after_join(1));
    assert(!empty.append({0, 1, 0, true, 1, 2}));
    assert(!empty.append({1, 0, 0, true, 1, 2}));
    assert(!empty.append({1, 1, 0, true, 0, 2}));
    assert(!empty.append({1, 1, 0, true, 3, 2}));
    assert(empty.invalid_after_join() == 4);
    assert(empty.dropped_after_join() == 0);
    assert(empty.append({1, 1, 0, true, 1, 2}));
    assert(!empty.complete_after_join());
    assert(!empty.matches_expected_after_join(1));
    CglCarrierDispatchTrace single_valid;
    assert(single_valid.append({1, 1, 0, true, 1, 2}));
    assert(single_valid.complete_after_join());
    assert(single_valid.matches_expected_after_join(1));
    assert(!single_valid.matches_expected_after_join(2));

    ActorState state;
    CglCarrierDispatchTrace trace;
    MultiActorCarrier carrier;
    const auto actor = carrier.register_actor(&state, finish_message, cleanup, &trace);
    auto producer = carrier.producer();
    carrier.start();
    constexpr std::uint64_t sent = CglCarrierDispatchTrace::capacity + 12;
    for (std::uint64_t i = 1; i <= sent; ++i) {
        assert(producer.try_send(actor, {i, static_cast<std::int64_t>(i)}));
        until_complete(state, i);
    }
    carrier.join();
    assert(state.completed.load() == sent);
    assert(state.canceled.load() == 1); // CONTROL/stop cleanup still takes precedence.
    assert(carrier.stats().protocol_failures == 0);
    assert(trace.recorded_after_join() == CglCarrierDispatchTrace::capacity);
    assert(trace.dropped_after_join() == 12);
    assert(trace.invalid_after_join() == 0);
    assert(!trace.complete_after_join());
    assert(!trace.matches_expected_after_join(CglCarrierDispatchTrace::capacity));
    std::array<CglCarrierDispatchTrace::Sample, CglCarrierDispatchTrace::capacity> samples{};
    assert(trace.copy_after_join(samples.data(), samples.size()) == samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& record = samples[i];
        assert(record.actor_slot == actor.slot);
        assert(record.actor_generation == actor.generation);
        assert(record.message_sequence == i + 1);
        assert(record.initial_slice);
        assert(record.slice_start_ns > 0);
        assert(record.slice_finish_ns >= record.slice_start_ns);
    }

    ActorState second;
    CglCarrierDispatchTrace second_trace;
    MultiActorCarrier next;
    const auto next_actor = next.register_actor(&second, finish_message, cleanup, &second_trace);
    auto next_producer = next.producer();
    next.start();
    assert(!next_producer.try_send(actor, {1, 0})); // old generation cannot replay.
    assert(next_producer.try_send(next_actor, {1, 0}));
    until_complete(second, 1);
    next.join();
    assert(second_trace.complete_after_join());
    assert(second_trace.recorded_after_join() == 1);
    assert(second_trace.dropped_after_join() == 0);
    assert(second_trace.matches_expected_after_join(1));
    assert(!second_trace.matches_expected_after_join(2));
    CglCarrierDispatchTrace::Sample single{};
    assert(second_trace.copy_after_join(&single, 1) == 1);
    assert(single.actor_generation == next_actor.generation);
    assert(single.actor_generation != actor.generation);
    return 0;
}
