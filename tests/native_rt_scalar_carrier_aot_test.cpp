#include "oreslang/runtime/HungryCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

extern "C" void oreslang_rt_scalar_frame_init(void*);
extern "C" int oreslang_rt_scalar_frame_step(void*, std::uint64_t);
extern "C" std::int64_t oreslang_rt_scalar_frame_result(void*);

using namespace oreslang::runtime;
using namespace std::chrono_literals;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, \
    "carrier invariant failed at %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); \
} } while (0)

struct Frame {
    std::uint64_t state = 0;
    std::uint64_t count = 0;
    std::uint64_t magic = 0;
};
struct State {
    Frame frames[64]{};
    std::atomic<std::uint64_t> thread{0};
    std::atomic<int> initials{0};
    std::atomic<int> resumes{0};
    std::atomic<int> completed{0};
    std::atomic<int> violations{0};
    std::atomic<int> io_calls{0};
};
static std::int64_t unexpected_io(void* raw, std::int64_t,
                                   const std::atomic<bool>&) noexcept {
    static_cast<State*>(raw)->io_calls.fetch_add(1);
    return 0;
}
static ActorStep frame_handler(void* raw, ActorMessage message,
                              ResumeCause cause, std::int64_t io) noexcept {
    auto& s = *static_cast<State*>(raw);
    auto err = [&] { s.violations.fetch_add(1); };
    auto id = message.id;
    if (id == 0 || id > 64 || io != 0) {
        err(); return {StepKind::done, 0, 0};
    }
    const std::uint64_t tag = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::uint64_t expected = 0;
    (void)s.thread.compare_exchange_strong(expected, tag);
    if (s.thread.load() != tag || HungryCarrier::active_heap() == nullptr) err();
    Frame& frame = s.frames[id - 1];
    if (cause == ResumeCause::initial) {
        // New user mail must never overtake a cooperative prior turn.
        if (s.completed.load() != static_cast<int>(id) - 1 || frame.state != 0) err();
        oreslang_rt_scalar_frame_init(&frame);
        s.initials.fetch_add(1);
    } else if (cause == ResumeCause::cpu) {
        if (frame.state != 1 || s.completed.load() != static_cast<int>(id) - 1)
            err();
        s.resumes.fetch_add(1);
    } else {
        err(); return {StepKind::done, 0, 0};
    }
    const auto before = frame.count;
    const int status = oreslang_rt_scalar_frame_step(&frame, 256);
    const std::uint64_t remaining = 1000 - before;
    const auto expected_steps = remaining < 256 ? remaining : 256;
    if (frame.count != before + expected_steps) err();
    if (status == 1 && frame.state == 1)
        return {StepKind::cooperate, 0, 0};
    if (status == 0 && frame.state == 2) {
        if (oreslang_rt_scalar_frame_result(&frame) != 1000) err();
        s.completed.fetch_add(1);
        return {StepKind::done, 0, 0};
    }
    err();
    return {StepKind::done, 0, 0};
}
template<class F>
static bool until(F pred, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}
static void real_compiled_guest_frame() {
    State state;
    HungryCarrier carrier(&state, frame_handler, &state, unexpected_io, {});
    carrier.start();
    const auto producer = carrier.producer_handle();
    for (std::uint64_t i = 1; i <= 64; ++i) {
        CHECK(until([&] { return producer.try_send({i, static_cast<std::int64_t>(i)}); },
                    1s));
    }
    CHECK(until([&] { return state.completed.load() == 64; }, 5s));
    auto before = carrier.stats();
    CHECK(before.accepted == 64);
    CHECK(before.completed == 64);
    CHECK(before.cooperations == 64 * 3);
    CHECK(before.io_completions == 0);
    CHECK(before.actor_thread_tag == state.thread.load());
    CHECK(state.initials.load() == 64);
    CHECK(state.resumes.load() == 64 * 3);
    CHECK(state.violations.load() == 0);
    CHECK(state.io_calls.load() == 0);
    carrier.join();
    const auto after = carrier.stats();
    CHECK(after.protocol_failures == 0);
    CHECK(after.abandoned_on_stop == 0);
}
struct BusyState {
    std::atomic<int> initial{0};
    std::atomic<int> cpu{0};
    std::atomic<int> violations{0};
};
static ActorStep busy_handler(void* raw, ActorMessage,
                              ResumeCause cause, std::int64_t) noexcept {
    auto& s = *static_cast<BusyState*>(raw);
    if (cause == ResumeCause::initial) {
        if (s.initial.fetch_add(1) != 0) s.violations.fetch_add(1);
    } else if (cause == ResumeCause::cpu) {
        if (s.initial.load() != 1) s.violations.fetch_add(1);
        s.cpu.fetch_add(1);
    } else {
        s.violations.fetch_add(1);
    }
    return {StepKind::cooperate, 0, 0};
}
static std::int64_t dummy_io(void*, std::int64_t, const std::atomic<bool>&) noexcept {
    return 0;
}
static void stop_preserves_unfinished_logical_turn() {
    BusyState state;
    HungryCarrier carrier(&state, busy_handler, nullptr, dummy_io, {});
    carrier.start();
    CHECK(carrier.producer_handle().try_send({1, 0}));
    CHECK(until([&] { return state.cpu.load() > 128; }, 2s));
    carrier.request_stop(); // CONTROL, independent of saturated user mailbox.
    carrier.join();
    const auto stats = carrier.stats();
    CHECK(state.initial.load() == 1);
    CHECK(state.violations.load() == 0);
    CHECK(stats.accepted == 1 && stats.completed == 0);
    CHECK(stats.abandoned_on_stop == 1);
    CHECK(stats.cooperations >= 129);
    CHECK(stats.io_completions == 0 && stats.protocol_failures == 0);
}
static ActorStep invalid_legacy(void*, ActorMessage, bool, std::int64_t) noexcept {
    return {StepKind::cooperate, 0, 0};
}
static ActorStep invalid_v2(void*, ActorMessage, ResumeCause,
                             std::int64_t) noexcept {
    return {StepKind::cooperate, 42, 1}; // no covert fake IO payload permitted
}
template<class Handler>
static void expect_protocol_rejection(Handler handler) {
    HungryCarrier carrier(nullptr, handler, nullptr, dummy_io, {});
    carrier.start();
    CHECK(carrier.producer_handle().try_send({1, 0}));
    CHECK(until([&] { return carrier.stopped(); }, 2s));
    carrier.join();
    const auto stats = carrier.stats();
    CHECK(stats.accepted == 1 && stats.completed == 0);
    CHECK(stats.protocol_failures == 1);
    CHECK(stats.io_completions == 0);
    CHECK(stats.cooperations == 0);
    CHECK(stats.abandoned_on_stop == 1);
}
int main() {
    real_compiled_guest_frame();
    stop_preserves_unfinished_logical_turn();
    expect_protocol_rejection(invalid_legacy);
    expect_protocol_rejection(invalid_v2);
    std::puts("NATIVE_COMPILED_SCALAR_CARRIER_COOPERATE_PASS");
    return 0;
}
