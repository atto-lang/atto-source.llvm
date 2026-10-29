#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/ActorLedgerLeaseAdapter.hpp"
#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using Ledger = oreslang::runtime::ActorExecutionLedger<4>;
using Adapter = oreslang::runtime::ActorLedgerLeaseAdapter<4>;
using Pool = oreslang::runtime::realtime::HardRealtimeTimerQueue<4>;

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "RT_TIMER_ACTOR_LEDGER_FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #x); std::abort(); \
} } while (0)

static void deadline_miss_is_not_guest_start() {
    Ledger actor(42, 7);
    Pool timers;
    Ledger::Token a{}, b{}, timer_event{};
    Ledger::Selection selected{};
    Pool::Handle timer{};
    Pool::Expired due{};
    Pool::Start start{};

    CHECK(actor.admit(Ledger::Kind::Mail, a) == Ledger::Status::Ok);
    CHECK(actor.admit(Ledger::Kind::Mail, b) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(actor.prepare_await(a) == Ledger::Status::Ok);
    CHECK(timers.arm_after(100, 5, 42, 7, 1, timer) == Pool::Status::Ok);
    CHECK(actor.suspend(a) == Ledger::Status::Ok);

    CHECK(timers.poll_due(105, due) == Pool::Status::Ok);
    CHECK(due.actor_id == 42 && due.actor_generation == 7);
    // A timer expiry is only readiness. Once an authenticated actor turn
    // ingests it, the timer is still serialized behind unresolved A.
    CHECK(actor.admit(Ledger::Kind::Timer, timer_event) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::NoEligible);

    CHECK(actor.wake(a) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(actor.finish_after_exit(a) == Ledger::Status::Ok);

    // Older ordinary mail B is already admitted. The current ledger has no
    // hard-RT priority lane, so B runs before the later timer event.
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == b.sequence);
    CHECK(actor.finish_after_exit(b) == Ledger::Status::Ok);

    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.kind == Ledger::Kind::Timer);
    std::uint32_t timer_guest_starts = 0;
    const auto start_status = timers.claim_start_with_dispatch_bound(
        timer, 42, 7, 108, 1, start);
    if (start_status == Pool::Status::Ok) {
        // This models the only legal branch into the timer guest body. A
        // mutation that accidentally admits the late start now causes an
        // observable execution, rather than passing a constant-false sentinel.
        ++timer_guest_starts;
    }
    CHECK(start_status == Pool::Status::DeadlineMissed);
    CHECK(start.deadline_ns == 105);
    CHECK(start.maximum_lateness_ns == 1);
    CHECK(start.lateness_ns == 3);
    CHECK(timer_guest_starts == 0);
    // Retire only the authenticated dispatch wrapper. No timer guest body ran.
    CHECK(actor.cancel(timer_event) == Ledger::Status::Ok);
    CHECK(actor.finish_after_exit(timer_event) == Ledger::Status::Ok);
    CHECK(timers.outstanding() == 0);
}

static void stop_revokes_timer_and_fences_generation() {
    Ledger actor(55, 9);
    Pool timers;
    Ledger::Token a{};
    Ledger::Selection selected{};
    Pool::Handle old_timer{}, replacement_timer{};
    Pool::Expired due{};
    Pool::Start start{};
    std::size_t revoked = 0;

    CHECK(actor.admit(Ledger::Kind::Mail, a) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(actor.prepare_await(a) == Ledger::Status::Ok);
    CHECK(timers.arm_after(200, 5, 55, 9, 2, old_timer)
          == Pool::Status::Ok);
    CHECK(actor.suspend(a) == Ledger::Status::Ok);

    // STOP/hot-reload revokes both sides of the old incarnation. The actor's
    // suspended frame becomes cleanup-only and the scheduler-owned timer loses
    // its ARMED/READY reservation before any late expiry can publish readiness.
    CHECK(actor.stop_and_request_cleanup() == Ledger::Status::Ok);
    CHECK(timers.revoke_actor(55, 9, revoked) == Pool::Status::Ok);
    CHECK(revoked == 1);
    CHECK(timers.outstanding() == 0);
    CHECK(timers.poll_due(205, due) == Pool::Status::NotReady);
    CHECK(timers.claim_start(old_timer, 55, 9, 205, start)
          == Pool::Status::Stale);
    CHECK(actor.wake(a) == Ledger::Status::AlreadyCancelled);

    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(selected.dispatch == Ledger::Dispatch::CancelCleanup);
    CHECK(actor.finish_after_exit(a) == Ledger::Status::Ok);
    CHECK(actor.physically_retired());

    // A replacement incarnation may reuse the timer pool, but the old actor
    // generation can neither claim nor poison the replacement timer.
    CHECK(timers.arm_after(205, 0, 55, 10, 0, replacement_timer)
          == Pool::Status::Ok);
    CHECK(timers.poll_due(205, due) == Pool::Status::Ok);
    CHECK(due.actor_id == 55 && due.actor_generation == 10);
    CHECK(timers.claim_start(replacement_timer, 55, 9, 205, start)
          == Pool::Status::Stale);
    CHECK(timers.claim_start(replacement_timer, 55, 10, 205, start)
          == Pool::Status::Ok);
    CHECK(timers.outstanding() == 0);
}

struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef refs[2]{};
    std::unique_ptr<Adapter> ledger;
    Adapter::Token a{}, b{};
    OresNativeWakeTicket wake{};
    OresNativeRetainedDataTicket retained{};
    Pool timers;
    Pool::Handle timer{};
    Pool::Expired due{};
    Pool::Start start{};
    std::atomic<int> suspended{0};
    std::atomic<int> other_progress{0};
    std::atomic<int> resumed_guest_start{0};
    std::atomic<int> later_mail{0};
    std::atomic<int> wake_finished{0};
};

extern "C" void rt_data(void* raw, std::size_t actor, std::uint64_t payload) {
    auto& f = *static_cast<Fixture*>(raw);
    CHECK(actor < 2);
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler, f.refs[actor]));
    if (actor == 1) {
        CHECK(payload == 9);
        f.other_progress.fetch_add(1, std::memory_order_release);
        return;
    }

    Adapter::Selection selected{};
    if (payload == 1) {
        CHECK(f.ledger->current_actor_lease());
        CHECK(f.ledger->admit(Adapter::Kind::Mail, f.a) == Adapter::Status::Ok);
        CHECK(f.ledger->claim(selected) == Adapter::Status::Ok);
        CHECK(selected.token.sequence == f.a.sequence);
        CHECK(ores_native_actor_scheduler_reserve_wake(
            f.scheduler, f.refs[0], f.refs[0], 1, 1, 1, 1, &f.wake));
        CHECK(f.ledger->prepare_await(f.a) == Adapter::Status::Ok);
        CHECK(f.timers.arm_after(100, 5, 1234, f.refs[0].generation, 2, f.timer)
              == Pool::Status::Ok);
        CHECK(f.ledger->suspend(f.a) == Adapter::Status::Ok);
        CHECK(ores_native_actor_scheduler_retain_current_data(
            f.scheduler, &f.retained));
        f.suspended.store(1, std::memory_order_release);
        return;
    }

    CHECK(payload == 2);
    CHECK(f.resumed_guest_start.load(std::memory_order_acquire) == 1);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler, f.refs[0]) == 0);
    CHECK(f.ledger->admit(Adapter::Kind::Mail, f.b) == Adapter::Status::Ok);
    CHECK(f.ledger->claim(selected) == Adapter::Status::Ok);
    CHECK(selected.token.sequence == f.b.sequence);
    CHECK(f.ledger->finish_after_guest_exit(f.b) == Adapter::Status::Ok);
    f.later_mail.fetch_add(1, std::memory_order_release);
}

extern "C" void rt_wake(void* raw, std::size_t actor,
                          OresNativeWakeTicket ticket,
                          OresNativeWakeOutcome outcome) {
    auto& f = *static_cast<Fixture*>(raw);
    CHECK(actor == 0);
    CHECK(outcome == ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f.scheduler, ticket));
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler, f.refs[0]));
    CHECK(f.ledger->current_actor_lease());

    CHECK(f.ledger->deliver_wake(f.a) == Adapter::Status::Ok);
    Adapter::Selection selected{};
    CHECK(f.ledger->claim(selected) == Adapter::Status::Ok);
    CHECK(selected.token.sequence == f.a.sequence);

    // This is the trusted seam immediately before the resumed guest segment.
    // The timestamp is deterministic test input, not hardware timing proof.
    // Deterministic 1ns reserve models bound-aware admission, not a
    // measured or qualified native first-instruction latency.
    CHECK(f.timers.claim_start_with_dispatch_bound(
        f.timer, 1234, f.refs[0].generation, 106, 1, f.start)
          == Pool::Status::Ok);
    CHECK(f.start.dispatch_bound_applied && f.start.dispatch_bound_ns == 1);
    CHECK(f.start.deadline_ns == 105);
    CHECK(f.start.maximum_lateness_ns == 2);
    CHECK(f.start.lateness_ns == 1);
    f.resumed_guest_start.fetch_add(1, std::memory_order_release);

    CHECK(f.ledger->finish_after_guest_exit(f.a) == Adapter::Status::Ok);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f.scheduler, f.retained));
    f.wake_finished.fetch_add(1, std::memory_order_release);
}

static void until(const std::atomic<int>& flag, const char* name) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "TIMEOUT: %s\n", name);
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void authenticated_timer_wake_starts_same_turn() {
    Fixture f;
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 8, 2, rt_data, nullptr, nullptr, rt_wake, &f);
    CHECK(f.scheduler);
    f.refs[0] = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    f.refs[1] = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    CHECK(f.refs[0].scheduler_domain && f.refs[1].scheduler_domain);
    f.ledger = std::make_unique<Adapter>(f.scheduler, f.refs[0], 1234);

    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.refs[0], 1));
    until(f.suspended, "actor A logical suspension");

    // Same-actor later mail remains retained behind A, while another actor
    // can progress on the scheduler.
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.refs[0], 2));
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.refs[1], 9));
    until(f.other_progress, "independent actor progress");
    CHECK(f.later_mail.load(std::memory_order_acquire) == 0);

    CHECK(f.timers.poll_due(105, f.due) == Pool::Status::Ok);
    CHECK(f.due.actor_id == 1234);
    CHECK(f.due.actor_generation == f.refs[0].generation);
    // Clock/producer context has no actor mutation authority.
    CHECK(f.ledger->deliver_wake(f.a) == Adapter::Status::Unauthorized);

    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler, f.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler, f.wake, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);

    until(f.wake_finished, "authenticated timer wake");
    until(f.later_mail, "later same-actor mail");
    CHECK(f.resumed_guest_start.load(std::memory_order_acquire) == 1);
    CHECK(f.timers.outstanding() == 0);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler, f.refs[0]) == 0);

    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

int main() {
    deadline_miss_is_not_guest_start();
    stop_revokes_timer_and_fences_generation();
    authenticated_timer_wake_starts_same_turn();
    std::puts("RT_TIMER_ACTOR_LEDGER_INTEGRATION_PASS");
    return 0;
}
