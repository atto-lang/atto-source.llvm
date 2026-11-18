#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
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
    OresNativeWakeTicket ticket{};
    std::atomic<int> wake_count{0};
    std::atomic<int> ready_auth{0};
    std::atomic<int> delivered{0};
    std::thread::id host_thread;
    std::atomic<int> invalid_mail_denied{0};
    std::atomic<int> cross_actor_denied{0};
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
                     bool stop_native = false) {
    State state;
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
                  {"gpu-actor", state.actor.generation}, "root", 256);
    state.bridge = &bridge;
    bridge.bind_native_actor(state.scheduler, state.actor);
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
    if (stop_native) {
        // The scheduler consumes a transferred wake when its producer STOPs;
        // the provider still had to establish physical event quiescence.
        check(state.wake_count.load() == 0,
              "retired actor must never execute a GPU completion");
        check(ores_native_actor_scheduler_outstanding_wake_loans(
            state.scheduler, state.actor) == 0,
              "native STOP left GPU wake escrow stranded");
        check(ores_native_actor_scheduler_wait_done(state.scheduler, state.actor),
              "native STOP did not reach physical DONE after device event");
    } else {
    check(state.wake_count.load() == 1, "terminal event must publish one wake");
    const auto expected = cancel ? ORES_NATIVE_WAKE_CANCELLED
        : (result == Port::Observation::CompleteOk
            ? ORES_NATIVE_WAKE_READY : ORES_NATIVE_WAKE_FAILED);
    check(state.delivered.load() == static_cast<int>(expected),
          "GPU completion had wrong outcome");
    check(state.ready_auth.load() == (expected == ORES_NATIVE_WAKE_READY),
          "GPU cancellation or failure leaked READY continuation authority");
    check(ores_native_actor_scheduler_outstanding_wake_loans(
        state.scheduler, state.actor) == 0, "GPU wake loan not retired");
    registry->progress_all(); // no duplicate WAKE on repeated polls
    check(ores_native_actor_scheduler_drain(state.scheduler), "duplicate wake drain");
    check(state.wake_count.load() == 1, "duplicate GPU event resumed twice");
    }
    if (!stop_native && !cancel && result == Port::Observation::CompleteOk)
        bridge.retire_gracefully();
    else registry->progress_all();
    ores_native_actor_scheduler_retire(state.scheduler);
    check(ores_native_actor_scheduler_try_destroy(state.scheduler),
          "native scheduler was not physically quiescent");
    std::printf("GPU_ACTOR_NATIVE_WAKE_PASS cancel=%d unknown=%d stopped=%d\n",
                cancel, unknown, stop_native);
}

int main() {
    run_case(Port::Observation::CompleteOk, false, false);
    run_case(Port::Observation::CompleteFailed, false, false);
    run_case(Port::Observation::CompleteOk, true, false);
    run_case(Port::Observation::CompleteOk, false, true);
    run_case(Port::Observation::CompleteOk, true, false, true);
    return 0;
}
