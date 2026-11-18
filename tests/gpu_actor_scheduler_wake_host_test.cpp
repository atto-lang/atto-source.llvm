#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::gpu_actor;
using Registry = GpuActorQuarantineRegistryV1;
using Port = ProviderPortV1;
using Bridge = GpuActorProviderBridgeV1;
using Lane = oreslang::SupervisorFailureLane;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

class MockProvider final : public Port {
public:
    explicit MockProvider(std::uint64_t generation)
        : Port({"gpu-actor", generation, "tenantA", "gpu0", "queueA", 2, 3, 5}) {}
    Observation observation = Observation::Pending;
    EventHandle event;
    bool unknown = false;
    EnqueueResult enqueue(Turn turn) override {
        if (unknown) return {EnqueueState::Unknown, nullptr};
        event = issue_event(turn);
        return {EnqueueState::Submitted, event};
    }
    EnqueueResult reconcile_unknown(Turn turn) override {
        event = issue_event(turn);
        return {EnqueueState::Submitted, event};
    }
    Observation observe(const EventHandle& candidate) override {
        check(candidate == event, "GPU poll observed foreign event");
        return observation;
    }
    bool try_release_buffer() override { return true; }
};

struct State {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    OresNativeActorRef sibling{}; // same generation, different slot
    Bridge* bridge = nullptr;
    Registry* registry = nullptr;
    OresNativeWakeTicket ticket{};
    std::atomic<int> wake_count{0};
    std::atomic<int> ready_auth{0};
    std::atomic<int> delivered{0};
    std::thread::id host_thread;
    std::atomic<int> invalid_mail_denied{0};
    std::atomic<int> cross_actor_denied{0};
    bool overflow_test = false;
    std::atomic<int> overflow_denied{0};
    std::atomic<int> carrier_poll_denied{0};
};

static void on_data(void* opaque, size_t actor, std::uint64_t payload) {
    try {
    auto& s = *static_cast<State*>(opaque);
    check(ores_native_actor_scheduler_in_any_actor_turn() != 0,
          "native GPU admission must be on an actor carrier");
    if (actor == 1) {
        // A foreign actor may have the exact same generation number as
        // the GPU actor. Reject its authentic DATA-mail WAKE regardless.
        check(payload == 2, "unexpected sibling message");
        std::uint64_t sibling_mail = 0;
        check(ores_native_actor_scheduler_current_data_mail_sequence(
            s.scheduler, s.sibling, &sibling_mail), "sibling mail not authenticated");
        OresNativeWakeTicket foreign{};
        check(ores_native_actor_scheduler_reserve_wake(
            s.scheduler, s.sibling, s.sibling, 11, 12, 13, 14, &foreign),
              "sibling wake reservation failed");
        try {
            (void)s.bridge->submit_with_native_wake(
                3, s.scheduler, foreign, sibling_mail);
        } catch (const std::logic_error& error) {
            if (std::string(error.what()) == "GPU_PROVIDER_FOREIGN_WAKE_AUTHORITY")
                s.cross_actor_denied.store(1, std::memory_order_release);
            else throw;
        }
        check(ores_native_actor_scheduler_cancel_wake(s.scheduler, foreign),
              "sibling denied wake remained reserved");
        return;
    }
    check(actor == 0 && payload == 1, "unexpected actor message");
    // GPU provider progress is host/poller work, not an actor turn. Reject
    // even when the registry is otherwise idle.
    try {
        s.registry->progress_all();
    } catch (const std::logic_error& e) {
        if (std::string(e.what()) == "GPU_PROVIDER_PROGRESS_ON_ACTOR_CARRIER")
            s.carrier_poll_denied.store(1, std::memory_order_release);
        else throw;
    }
    check(s.carrier_poll_denied.load(std::memory_order_acquire) == 1,
          "GPU provider polling escaped onto actor carrier");
    std::uint64_t mail = 0;
    check(ores_native_actor_scheduler_current_data_mail_sequence(
        s.scheduler, s.actor, &mail), "missing authenticated DATA mail");
    check(ores_native_actor_scheduler_reserve_wake(
        s.scheduler, s.actor, s.actor, 11, 12, 13, 14, &s.ticket),
          "unable to reserve GPU continuation");
    check(ores_native_actor_scheduler_current_data_issued_wake(
        s.scheduler, s.ticket, mail), "wake not bound to this DATA mail");
    try {
        (void)s.bridge->submit_with_native_wake(
            3, s.scheduler, s.ticket, mail + 1);
    } catch (const std::logic_error& error) {
        if (std::string(error.what()) == "GPU_PROVIDER_FOREIGN_WAKE_AUTHORITY")
            s.invalid_mail_denied.store(1, std::memory_order_release);
        else throw;
    }
    check(s.invalid_mail_denied.load(std::memory_order_acquire) == 1,
          "forged DATA-mail provenance accepted");
    if (s.overflow_test) {
        // Overflow fails before host fence publication, and MUST release the
        // transferred wake escrow so a retry can reserve a fresh ticket.
        try {
            (void)s.bridge->submit_with_native_wake(
                std::numeric_limits<std::int64_t>::max(),
                s.scheduler, s.ticket, mail);
        } catch (const std::overflow_error& e) {
            if (std::string(e.what()) == "GPU_ACTOR_STAGE_OVERFLOW")
                s.overflow_denied.store(1, std::memory_order_release);
            else throw;
        }
        check(s.overflow_denied.load(std::memory_order_acquire) == 1,
              "GPU staging overflow was not denied");
        check(ores_native_actor_scheduler_outstanding_wake_loans(
            s.scheduler, s.actor) == 0,
              "pre-enqueue failure stranded a native wake");
        check(s.bridge->committed_state() == 1,
              "pre-enqueue failure incorrectly committed host state");
        check(ores_native_actor_scheduler_reserve_wake(
            s.scheduler, s.actor, s.actor, 15, 16, 17, 18, &s.ticket),
              "native WAKE cannot be re-reserved after overflow");
    }
    (void)s.bridge->submit_with_native_wake(3, s.scheduler, s.ticket, mail);
    } catch (...) { std::abort(); } // Never unwind C++ exceptions through C scheduler frames.
}

static void on_control(void*, size_t, std::uint64_t) {
    std::abort(); // Unexpected native CONTROL in this test.
}
static void on_wake(void* opaque, size_t actor, OresNativeWakeTicket ticket,
                    OresNativeWakeOutcome outcome) {
    try {
    auto& s = *static_cast<State*>(opaque);
    check(actor == 0, "foreign actor resumed");
    check(std::this_thread::get_id() != s.host_thread,
          "GPU polling thread executed actor callback");
    check(ores_native_actor_scheduler_is_current_wake(s.scheduler, ticket),
          "stale or unauthenticated GPU wake entered actor");
    check(ores_native_actor_scheduler_in_any_actor_turn(),
          "GPU completion bypassed actor scheduling");
    s.ready_auth.store(ores_native_actor_scheduler_is_current_ready_wake(
        s.scheduler, ticket), std::memory_order_release);
    s.delivered.store(static_cast<int>(outcome), std::memory_order_release);
    ++s.wake_count;
    } catch (...) { std::abort(); } // No exception may cross the native C ABI.
}

static void run_case(Port::Observation result, bool cancel, bool unknown,
                     bool stop_native = false, bool overflow_test = false,
                     bool device_loss_first = false) {
    State state;
    state.overflow_test = overflow_test;
    state.host_thread = std::this_thread::get_id();
    state.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 4, 2, on_data, on_control, nullptr, on_wake, &state);
    check(state.scheduler != nullptr, "native scheduler failed to open");
    state.actor = ores_native_actor_scheduler_spawn(state.scheduler, 0);
    check(state.actor.generation != 0, "native actor failed to spawn");
    state.sibling = ores_native_actor_scheduler_spawn(state.scheduler, 1);
    check(state.sibling.generation == state.actor.generation
        && state.sibling.actor_slot != state.actor.actor_slot,
          "expected two separate actors with same generation");

    auto registry = std::make_shared<Registry>(4);
    auto provider = std::make_shared<MockProvider>(state.actor.generation);
    provider->unknown = unknown;
    auto control = std::make_shared<Lane>(4);
    Bridge bridge(registry, provider, control,
                  {"gpu-actor", state.actor.generation}, "root", 256,
                  overflow_test ? 1 : 0);
    state.bridge = &bridge;
    state.registry = registry.get();
    bridge.bind_native_actor(state.scheduler, state.actor);
    // Bound native GPU actors must never enter the host-only submission
    // path: a WAKE-free submit could strand logical Future/STOP ownership.
    try {
        (void)bridge.submit(3);
        check(false, "native GPU bridge allowed wake-free submission");
    } catch (const std::logic_error& error) {
        check(std::string(error.what()) == "GPU_PROVIDER_NATIVE_WAKE_REQUIRED",
              "native GPU wake-free denial returned wrong error");
    }
    try {
        bridge.bind_native_actor(state.scheduler, state.sibling);
        check(false, "native binding incorrectly allowed retarget");
    } catch (const std::logic_error& error) {
        check(std::string(error.what()) == "GPU_PROVIDER_NATIVE_BINDING_DENIED",
              "native binding rejected with incorrect reason");
    }
    check(ores_native_actor_scheduler_try_send(
        state.scheduler, state.sibling, 2) == ORES_NATIVE_SEND_ACCEPTED,
          "sibling actor DATA send rejected");
    check(ores_native_actor_scheduler_drain(state.scheduler),
          "sibling DATA did not finish");
    check(state.cross_actor_denied.load(std::memory_order_acquire) == 1,
          "same-generation foreign actor wake crossed binding");
    check(ores_native_actor_scheduler_try_send(
        state.scheduler, state.actor, 1) == ORES_NATIVE_SEND_ACCEPTED,
          "actor DATA send rejected");
    check(ores_native_actor_scheduler_drain(state.scheduler), "actor DATA did not finish");
    check(state.wake_count.load() == 0, "GPU submission ran completion inline");
    check(ores_native_actor_scheduler_outstanding_wake_loans(
        state.scheduler, state.actor) == 1, "GPU continuation wake loan not retained");
    // Unknown enqueue never publishes an optimistic wake. Reconcile first.
    registry->progress_all();
    check(state.wake_count.load() == 0, "device Pending/Unknown caused a wake");
    if (device_loss_first) {
        provider->observation = Port::Observation::DeviceLost;
        registry->progress_all();
        check(state.wake_count.load() == 0,
              "device loss fabricated a completion wake");
        check(ores_native_actor_scheduler_outstanding_wake_loans(
            state.scheduler, state.actor) == 1,
              "device loss freed a still-unproven GPU continuation");
        // Mock simulates a later independently authenticated physical
        // quiescence witness; real driver recovery cannot simply flip this
        // status without verifying the underlying device and event.
    }
    if (stop_native) {
        // Physical STOP cannot interpret an in-flight GPU cancellation as
        // permission to reclaim the device loan or resume guest code.
        check(ores_native_actor_scheduler_stop_sealed(state.scheduler, state.actor),
              "actor native STOP refused");
        bridge.request_close();
    } else if (cancel) bridge.request_close();
    provider->observation = result;
    registry->progress_all();
    check(ores_native_actor_scheduler_drain(state.scheduler),
          "resumed native actor did not quiesce");
    const bool terminal_gpu_actor = stop_native || cancel || device_loss_first
        || result == Port::Observation::CompleteFailed;
    if (terminal_gpu_actor) {
        // A closed GPU-ACTOR incarnation must never execute guest code on
        // a native WAKE, including a previously enqueued completion.
        check(state.wake_count.load() == 0,
              "terminal GPU actor executed a continuation");
        check(ores_native_actor_scheduler_outstanding_wake_loans(
            state.scheduler, state.actor) == 0,
              "terminal GPU actor left WAKE escrow live");
        check(ores_native_actor_scheduler_wait_done(state.scheduler, state.actor),
              "terminal GPU actor failed to reach native DONE");
        const auto notice = control->poll();
        check(notice.has_value(), "terminal GPU actor emitted no supervisor CONTROL");
        check(notice->kind == (device_loss_first
                || result == Port::Observation::CompleteFailed
                ? Lane::Kind::HostFailure : Lane::Kind::Cancel),
              "terminal GPU actor produced incorrect CONTROL kind");
        control->ack(notice->actor);
    } else {
        check(state.wake_count.load() == 1,
              "successful GPU event must publish one actor WAKE");
        check(state.delivered.load() == ORES_NATIVE_WAKE_READY,
              "successful GPU event yielded a non-READY wake");
        check(state.ready_auth.load() == 1,
              "READY wake did not have native actor execution authority");
        check(ores_native_actor_scheduler_outstanding_wake_loans(
            state.scheduler, state.actor) == 0,
              "successful GPU event left a native wake loan");
        registry->progress_all();
        check(ores_native_actor_scheduler_drain(state.scheduler),
              "duplicate GPU wake drain");
        check(state.wake_count.load() == 1,
              "duplicate GPU event resumed twice");
    }
    if (!terminal_gpu_actor)
        bridge.retire_gracefully();
    else registry->progress_all();
    ores_native_actor_scheduler_retire(state.scheduler);
    check(ores_native_actor_scheduler_try_destroy(state.scheduler),
          "native scheduler was not physically quiescent");
    std::printf("GPU_ACTOR_NATIVE_WAKE_PASS cancel=%d unknown=%d stopped=%d lost=%d\n",
                cancel, unknown, stop_native, device_loss_first);
}

int main() {
    run_case(Port::Observation::CompleteOk, false, false);
    run_case(Port::Observation::CompleteFailed, false, false);
    run_case(Port::Observation::CompleteOk, true, false);
    run_case(Port::Observation::CompleteOk, false, true);
    run_case(Port::Observation::CompleteOk, true, false, true);
    run_case(Port::Observation::CompleteOk, false, false, false, true);
    run_case(Port::Observation::CompleteOk, false, false, false, false, true);
    return 0;
}
