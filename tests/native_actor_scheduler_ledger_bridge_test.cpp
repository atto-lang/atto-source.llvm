#include "oreslang/runtime/NativeActorLedgerBridge.hpp"
#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using Bridge = oreslang::runtime::NativeActorLedgerBridge<8>;
using Pool = oreslang::runtime::realtime::HardRealtimeTimerQueue<4>;
using Id = Bridge::Id;
struct State {
    OresNativeActorScheduler* native = nullptr;
    OresNativeActorRef a{}, b{};
    std::atomic<Bridge*> bridge{nullptr};
    std::atomic<std::uint64_t> suspended_seq{0}, timer_seq{0};
    std::atomic<int> first_called{0}, b_progress{0}, hold_first{1};
    std::atomic<int> trace_count{0}, errors{0}, supervisory{0};
    int trace[4]{}; // actor A's single-writer carrier owns writes
};
static void step(State& s, int expected, int label) noexcept {
    const int index = s.trace_count.load(std::memory_order_relaxed);
    if (index != expected || index >= 4) { s.errors.fetch_add(1); return; }
    s.trace[index] = label;
    s.trace_count.store(index + 1, std::memory_order_release);
}
static Bridge::Decision process(void* raw, Id id, Bridge::Kind kind,
                                std::uint64_t payload) noexcept {
    auto& s = *static_cast<State*>(raw);
    auto* bridge = s.bridge.load(std::memory_order_acquire);
    if (!bridge || !bridge->actor_authorized()) { s.errors.fetch_add(1); return Bridge::Decision::Finish; }
    if (kind == Bridge::Kind::Control) {
        s.supervisory.fetch_add(1);
        return Bridge::Decision::Finish;
    }
    if (kind == Bridge::Kind::Timer) {
        step(s, 1, 2);
        return Bridge::Decision::Finish;
    }
    if (payload == 100) {
        if (s.first_called.fetch_add(1) == 0) {
            Id timer{};
            if (bridge->admit_timer(timer) != Bridge::Status::Ok)
                s.errors.fetch_add(1);
            s.timer_seq.store(timer.sequence, std::memory_order_release);
            s.suspended_seq.store(id.sequence, std::memory_order_release);
            // TEST ONLY: hold the physical actor lease to force a pending
            // supervisor CONTROL slot and independently progress peer actor B.
            while (s.hold_first.load(std::memory_order_acquire))
                std::this_thread::yield();
            return Bridge::Decision::Suspend;
        }
        step(s, 0, 1);
    } else if (payload == 200) step(s, 2, 3);
    else s.errors.fetch_add(1);
    return Bridge::Decision::Finish;
}
static void on_mail(void* raw, std::size_t actor, std::uint64_t payload) {
    auto& s = *static_cast<State*>(raw);
    if (actor == 1) {
        if (!ores_native_actor_scheduler_is_current_actor(s.native, s.b)) s.errors.fetch_add(1);
        s.b_progress.fetch_add(1);
    } else if (actor == 0) {
        auto* b = s.bridge.load(std::memory_order_acquire);
        if (!b) s.errors.fetch_add(1);
        else b->on_message(payload);
    } else s.errors.fetch_add(1);
}
static void on_control(void* raw, std::size_t actor, std::uint64_t payload) {
    auto& s = *static_cast<State*>(raw);
    if (actor != 0) { s.errors.fetch_add(1); return; }
    auto* b = s.bridge.load(std::memory_order_acquire);
    if (!b) { s.errors.fetch_add(1); return; }
    if (!b->on_control(payload)) {
        // A non-timer CONTROL payload remains supervisor-owned; it can run
        // with exclusive native carrier authority without overtaking A's await.
        if (!b->on_supervisor_control(payload)) s.errors.fetch_add(1);
    }
}
static bool until(const std::atomic<int>& value, int expected) {
    for (int i = 0; i < 2000; ++i) {
        if (value.load(std::memory_order_acquire) >= expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
int main() {
    State s{};
    s.native = ores_native_actor_scheduler_open_control(2, 4, 2, on_mail, on_control, &s);
    assert(s.native);
    assert(ores_native_actor_scheduler_prestart_carriers(s.native));
    s.a = ores_native_actor_scheduler_spawn(s.native, 0);
    s.b = ores_native_actor_scheduler_spawn(s.native, 1);
    assert(s.a.scheduler_domain && s.b.scheduler_domain);
    auto owner = std::make_unique<Bridge>(s.native, s.a, process, &s);
    auto* bridge = owner.get();
    s.bridge.store(bridge, std::memory_order_release);

    assert(bridge->send(100) == Bridge::Submit::Accepted);
    for (int i = 0; !s.suspended_seq.load(std::memory_order_acquire) && i < 2000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.suspended_seq.load(std::memory_order_acquire));
    assert(s.timer_seq.load(std::memory_order_acquire));
    // An actor can be RUNNING while a separate peer makes progress; a pending
    // supervisor CONTROL reservation blocks ordinary timer CONTROL admission.
    assert(bridge->send(200) == Bridge::Submit::Accepted);
    assert(ores_native_actor_scheduler_send(s.native, s.b, 501));
    assert(until(s.b_progress, 1));
    assert(ores_native_actor_scheduler_publish_control(s.native, s.a, 777));

    // Main thread stands in for a single-owner CLOCK driver; it never runs
    // the actor guest hook or dereferences an actor-private continuation.
    Pool pool;
    Pool::Handle h{};
    Pool::Expired due{};
    assert(pool.arm_after(100, 5, 1, s.a.generation, 3, h) == Pool::Status::Ok);
    assert(pool.poll_due(105, due) == Pool::Status::Ok);
    assert(due.actor_id == 1 && due.actor_generation == s.a.generation);
    auto notify_timer = bridge->notify_ready(Id{1, s.a.generation,
        s.timer_seq.load(std::memory_order_acquire), s.a.scheduler_domain});
    assert(notify_timer == Bridge::Notify::RetryRequired);
    auto notify_future = bridge->notify_ready(Id{1, s.a.generation,
        s.suspended_seq.load(std::memory_order_acquire), s.a.scheduler_domain});
    assert(notify_future == Bridge::Notify::RetryRequired);
    assert(bridge->retry_needed());
    assert(bridge->notify_ready(Id{1, s.a.generation, 1,
        s.a.scheduler_domain + 1}) == Bridge::Notify::Invalid);
    assert(s.trace_count.load() == 0);
    s.hold_first.store(0, std::memory_order_release);
    assert(until(s.supervisory, 1));  // reserved supervisor CONTROL wins
    for (int i = 0; bridge->messages_arrived() < 2 && i < 2000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(bridge->messages_arrived() == 2);
    assert(s.trace_count.load() == 0); // B can't overtake suspended A
    for (int i = 0; i < 2000 && s.trace_count.load(std::memory_order_acquire) < 3; ++i) {
        (void)bridge->retry_kick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(until(s.trace_count, 3));
    assert(s.trace[0] == 1 && s.trace[1] == 2 && s.trace[2] == 3);
    Pool::Start late{};
    assert(pool.claim_start(h, 1, s.a.generation, 109, late)
           == Pool::Status::DeadlineMissed);
    assert(late.lateness_ns == 4 && late.maximum_lateness_ns == 3);

    // Late duplicated wakes are ACKed as stale, not reexecuted.
    (void)bridge->notify_ready(Id{1, s.a.generation, s.suspended_seq.load(), s.a.scheduler_domain});
    (void)bridge->retry_kick();
    assert(ores_native_actor_scheduler_drain(s.native));
    assert(s.first_called.load() == 2);
    assert(s.errors.load() == 0);
    assert(bridge->failures() == 0);
    assert(bridge->reserved() == 0);
    assert(ores_native_actor_scheduler_stop(s.native, s.a));
    assert(ores_native_actor_scheduler_stop(s.native, s.b));
    assert(bridge->retire_after_native_done());
    assert(ores_native_actor_scheduler_poll_done(s.native, s.a));
    ores_native_actor_scheduler_retire(s.native);
    assert(ores_native_actor_scheduler_try_destroy(s.native));
    std::cout << "NATIVE_ACTOR_LEDGER_REAL_CARRIER_PASS\n";
}
