#include "oreslang/runtime/NativeActorLedgerBridge.hpp"
#include "oreslang/runtime/NativeActorAwaitLedgerLink.hpp"
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
using AwaitLink = oreslang::runtime::NativeActorAwaitLedgerLink<Bridge>;
struct State {
    OresNativeActorScheduler* native = nullptr;
    OresNativeActorRef a{}, b{};
    std::atomic<Bridge*> bridge{nullptr};
    std::atomic<AwaitLink*> await_link{nullptr};
    OresNativeExecutionContextStore* executions = nullptr;
    OresNativeContinuationStore* continuations = nullptr;
    AwaitLink::Ticket await_ticket{};
    std::atomic<int> ticket_published{0};
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
            auto* link = s.await_link.load(std::memory_order_acquire);
            if (!link || !link->suspend(id, 9, 100) ||
                !link->ticket_for_actor(s.await_ticket)) {
                s.errors.fetch_add(1);
            } else {
                s.ticket_published.store(1, std::memory_order_release);
            }
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
        auto* link = s.await_link.load(std::memory_order_acquire);
        OresNativeExecutionContext resumed{};
        if (!link || !link->resume_begin(id, &resumed) ||
            resumed.id.runtime_domain != s.a.scheduler_domain ||
            resumed.allocation.owner != s.a.actor_slot ||
            resumed.allocation.generation == 0)
            s.errors.fetch_add(1);
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
    // Native guest handler has physically returned to its enclosing actor
    // carrier; only now may the continuation trace be finished and unpinned.
    auto* link = s.await_link.load(std::memory_order_acquire);
    if (link && link->state_for_actor() == AwaitLink::State::Resuming &&
        !link->finish_after_guest_return())
        s.errors.fetch_add(1);
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
    s.executions = ores_native_execution_context_store_open(16);
    assert(s.executions);
    s.continuations = ores_native_continuation_store_open(8, s.executions);
    assert(s.continuations);
    s.native = ores_native_actor_scheduler_open_control(2, 4, 2, on_mail, on_control, &s);
    assert(s.native);
    assert(ores_native_actor_scheduler_prestart_carriers(s.native));
    s.a = ores_native_actor_scheduler_spawn(s.native, 0);
    s.b = ores_native_actor_scheduler_spawn(s.native, 1);
    assert(s.a.scheduler_domain && s.b.scheduler_domain);
    auto owner = std::make_unique<Bridge>(s.native, s.a, process, &s);
    auto* bridge = owner.get();
    s.bridge.store(bridge, std::memory_order_release);
    auto await_owner = std::make_unique<AwaitLink>(
        *bridge, s.native, s.executions, s.continuations);
    s.await_link.store(await_owner.get(), std::memory_order_release);

    assert(bridge->send(100) == Bridge::Submit::Accepted);
    for (int i = 0; !s.suspended_seq.load(std::memory_order_acquire) && i < 2000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.suspended_seq.load(std::memory_order_acquire));
    assert(s.timer_seq.load(std::memory_order_acquire));
    assert(until(s.ticket_published, 1));
    OresNativeContinuationRecord original{};
    assert(ores_native_continuation_store_get(
        s.continuations, s.await_ticket.link.continuation, &original));
    assert(original.state == ORES_CONTINUATION_PENDING);
    assert(original.code_generation == 9 && original.source_token == 100);
    assert(original.execution_id.runtime_domain == s.a.scheduler_domain);
    assert(original.allocation.owner == s.a.actor_slot);
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
    auto notify_future = AwaitLink::publish_ready(
        s.continuations, *bridge, s.await_ticket);
    assert(notify_future == AwaitLink::Wake::Accepted);
    assert(!ores_native_continuation_store_notify_ready(
        s.continuations, s.await_ticket.link.continuation)); // no duplicate READY
    assert(bridge->retry_needed());
    assert(bridge->notify_ready(Id{1, s.a.generation, 1,
        s.a.scheduler_domain + 1}) == Bridge::Notify::Invalid);
    assert(s.trace_count.load() == 0);
    s.hold_first.store(0, std::memory_order_release);
    assert(until(s.supervisory, 1));  // reserved supervisor CONTROL wins
    // No manual retry_kick: supervisor CONTROL completion must rearm retained
    // timer/Future notices automatically under the authenticated actor lease.
    assert(until(s.trace_count, 3));
    assert(bridge->messages_arrived() == 2);
    assert(s.trace[0] == 1 && s.trace[1] == 2 && s.trace[2] == 3);
    Pool::Start late{};
    assert(pool.claim_start(h, 1, s.a.generation, 109, late)
           == Pool::Status::DeadlineMissed);
    assert(late.lateness_ns == 4 && late.maximum_lateness_ns == 3);

    // Late duplicated wakes are ACKed as stale, not reexecuted.
    (void)bridge->notify_ready(Id{1, s.a.generation, s.suspended_seq.load(), s.a.scheduler_domain});
    (void)bridge->retry_kick();
    assert(ores_native_actor_scheduler_drain(s.native));
    assert(await_owner->state_for_actor() == AwaitLink::State::Finished);
    assert(ores_native_continuation_store_live(s.continuations) == 0);
    assert(ores_native_execution_context_store_live(s.executions) == 0);
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
    assert(ores_native_continuation_store_close(s.continuations));
    assert(ores_native_execution_context_store_close(s.executions));
    std::cout << "NATIVE_ACTOR_LEDGER_REAL_CARRIER_PASS\n";
}
