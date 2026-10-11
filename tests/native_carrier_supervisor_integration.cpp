#include "oreslang/runtime/HungryCarrier.hpp"
#include "oreslang/runtime/EffectSupervisorBridge.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace oreslang;
using namespace oreslang::runtime;
using namespace std::chrono_literals;

#define REQUIRE(value) do { if (!(value)) throw std::runtime_error("failed: " #value); } while (false)

struct ParentState {
    std::atomic<bool> first_entered{false};
    std::atomic<bool> release_first{false};
    std::atomic<bool> saw_control{false};
    std::atomic<bool> second_done{false};
    std::atomic<bool> wrong_order{false};
    std::atomic<std::uint64_t> thread_tag{0};
    std::atomic<std::uint64_t> hook_tag{0};
};

static ActorStep parent_receive(void* raw, ActorMessage message,
                                bool resumed, std::int64_t) noexcept {
    auto& state = *static_cast<ParentState*>(raw);
    if (resumed) state.wrong_order.store(true, std::memory_order_release);
    if (message.id == 1) {
        state.thread_tag.store(
            std::hash<std::thread::id>{}(std::this_thread::get_id()),
            std::memory_order_release);
        state.first_entered.store(true, std::memory_order_release);
        while (!state.release_first.load(std::memory_order_acquire))
            std::this_thread::yield();
    } else if (message.id == 2) {
        if (!state.saw_control.load(std::memory_order_acquire))
            state.wrong_order.store(true, std::memory_order_release);
        state.second_done.store(true, std::memory_order_release);
    } else {
        state.wrong_order.store(true, std::memory_order_release);
    }
    return {StepKind::done, 0, 0};
}

struct ParentHooks final : SupervisorControlTurn::Hooks {
    ParentState& state;
    explicit ParentHooks(ParentState& s) : state(s) {}
    SupervisorControlTurn::Decision on_throw(const SupervisorFailureLane::Failure&) override {
        return SupervisorControlTurn::Decision::Escalate;
    }
    SupervisorControlTurn::Decision on_raise(const SupervisorFailureLane::Failure&) override {
        return SupervisorControlTurn::Decision::Escalate;
    }
    SupervisorControlTurn::Decision on_panic(const SupervisorFailureLane::Failure& f) override {
        if (f.summary != "cleanup panic" || f.actor.id != "child-native" ||
            f.actor.generation != 7) {
            state.wrong_order.store(true, std::memory_order_release);
        }
        state.hook_tag.store(
            std::hash<std::thread::id>{}(std::this_thread::get_id()),
            std::memory_order_release);
        state.saw_control.store(true, std::memory_order_release);
        return SupervisorControlTurn::Decision::Acknowledge;
    }
    SupervisorControlTurn::Decision on_cancel(const SupervisorFailureLane::Failure&) override {
        return SupervisorControlTurn::Decision::Escalate;
    }
    SupervisorControlTurn::Decision on_host_failure(const SupervisorFailureLane::Failure&) override {
        return SupervisorControlTurn::Decision::Escalate;
    }
    void on_escalate(const SupervisorFailureLane::Failure&) override {
        state.wrong_order.store(true, std::memory_order_release);
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override { return false; }
    void on_user_turn() override {
        state.wrong_order.store(true, std::memory_order_release);
    }
};

static std::int64_t broker(void*, std::int64_t value,
                           const std::atomic<bool>&) noexcept {
    return value;
}

struct ChildState {
    SupervisorControlTurn& supervisor;
    HungryCarrier::ProducerHandle wake;
    std::atomic<bool> reported{false};
    std::atomic<bool> failed{false};
};

static EffectStatus cleanup_panic(void*) noexcept {
    return {EffectKind::Panic, 42};
}

static ActorStep child_receive(void* raw, ActorMessage, bool resumed,
                               std::int64_t) noexcept {
    auto& child = *static_cast<ChildState*>(raw);
    if (resumed) child.failed.store(true, std::memory_order_release);
    EffectUnwind<2> unwind;
    if (!unwind.push(&cleanup_panic, nullptr)) {
        child.failed.store(true, std::memory_order_release);
        return {StepKind::done, 0, 0};
    }
    const auto outcome = unwind.finish({EffectKind::Throw, 7});
    if (outcome.primary.kind != EffectKind::Panic ||
        outcome.secondary_count != 1) {
        child.failed.store(true, std::memory_order_release);
        return {StepKind::done, 0, 0};
    }
    try {
        bool published = EffectSupervisorBridge::publish_terminal(
            child.supervisor, {"child-native", 7},
            outcome.primary, "cleanup panic", "parent-native");
        if (!published || !child.wake.try_wake_control())
            child.failed.store(true, std::memory_order_release);
    } catch (...) {
        child.failed.store(true, std::memory_order_release);
    }
    child.reported.store(true, std::memory_order_release);
    return {StepKind::done, 0, 0};
}

template <typename Predicate>
static bool within(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

int main() {
    ParentState parent_state;
    SupervisorControlTurn control(1);
    control.reserve_child({"child-native", 7});
    ParentHooks hooks(parent_state);

    HungryCarrier::Options opts;
    opts.supervisor_control = &control;
    opts.supervisor_hooks = &hooks;
    HungryCarrier parent(&parent_state, &parent_receive, nullptr, &broker, opts);
    auto parentProducer = parent.producer_handle();
    ChildState child_state{control, parentProducer};
    HungryCarrier child(&child_state, &child_receive, nullptr,
                        &broker, HungryCarrier::Options{});
    auto childProducer = child.producer_handle();

    parent.start();
    child.start();
    REQUIRE(parentProducer.try_send({1, 100}));
    REQUIRE(within([&] { return parent_state.first_entered.load(); }));
    REQUIRE(parentProducer.try_send({2, 200}));
    REQUIRE(childProducer.try_send({1, 50}));
    bool reported = within([&] { return child_state.reported.load(); });
    parent_state.release_first.store(true, std::memory_order_release);
    REQUIRE(reported);
    REQUIRE(within([&] { return parent_state.second_done.load(); }));
    REQUIRE(parent_state.saw_control.load());
    REQUIRE(!parent_state.wrong_order.load());
    REQUIRE(!child_state.failed.load());
    REQUIRE(parent_state.hook_tag.load() == parent_state.thread_tag.load());
    REQUIRE(parent_state.hook_tag.load() !=
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    REQUIRE(control.pending_count() == 0);
    REQUIRE(control.reserved_count() == 0);
    child.join();
    parent.join();
    REQUIRE(child.stats().completed == 1);
    REQUIRE(parent.stats().completed == 2);
    return 0;
}
