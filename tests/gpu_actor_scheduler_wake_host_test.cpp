#include "oreslang/runtime/GpuActorCompletionExecutorV1.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
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

class MockProvider : public Port {
public:
    explicit MockProvider(std::uint64_t generation,
                          std::string actor_id = "gpu-actor",
                          std::string tenant = "tenantA")
        : Port({std::move(actor_id), generation, std::move(tenant),
                "gpu0", "queueA", 2, 3, 5}),
          host_thread(std::this_thread::get_id()) {}
    const std::thread::id host_thread;
    std::atomic<int> foreign_host_polls{0};
    std::atomic<int> observed_events{0};
    std::atomic<Observation> observation{Observation::Pending};
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
        if (std::this_thread::get_id() == host_thread)
            ++foreign_host_polls;
        ++observed_events;
        check(candidate == event, "GPU poll observed foreign event");
        return observation.load(std::memory_order_acquire);
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
    // GPU completion is deliberately processed by a bounded native host
    // service, never the root CONTROL or an actor-carrier thread.
    GpuActorCompletionExecutorV1 executor(provider->binding(), 4);
    const auto progress = [&] {
        check(executor.request(registry, bridge.completion_lease_id()),
              "deduplicated GPU completion request was not admitted");
        executor.await_idle();
        check(executor.stats().failed == 0,
              "GPU completion executor caught a host exception");
    };
    // Unknown enqueue never publishes an optimistic wake. Reconcile first.
    progress();
    check(state.wake_count.load() == 0, "device Pending/Unknown caused a wake");
    if (device_loss_first) {
        provider->observation = Port::Observation::DeviceLost;
        progress();
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
    progress();
    check(provider->foreign_host_polls.load() == 0,
          "GPU event was observed on the root/control test thread");
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
        progress(); // no duplicate WAKE after a repeated terminal poll
        check(ores_native_actor_scheduler_drain(state.scheduler),
              "duplicate GPU wake drain");
        check(state.wake_count.load() == 1,
              "duplicate GPU event resumed twice");
    }
    if (!terminal_gpu_actor)
        bridge.retire_gracefully();
    else progress();
    check(executor.stats().processed >= (device_loss_first ? 3u : 2u),
          "bounded completion worker failed to process provider events");
    ores_native_actor_scheduler_retire(state.scheduler);
    check(ores_native_actor_scheduler_try_destroy(state.scheduler),
          "native scheduler was not physically quiescent");
    std::printf("GPU_ACTOR_NATIVE_WAKE_PASS cancel=%d unknown=%d stopped=%d lost=%d\n",
                cancel, unknown, stop_native, device_loss_first);
}

// Completion scheduling must be per exact device queue/lease; the executor
// coalesces event notifications and bounds retained GPU host authority even
// when a native driver/poller blocks. It is NOT GPU device evidence.
class BlockedPollProvider final : public MockProvider {
public:
    using MockProvider::MockProvider;
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool proceed = false;

    Observation observe(const EventHandle& event) override {
        {
            std::unique_lock<std::mutex> guard(mu);
            entered = true;
            cv.notify_all();
            cv.wait(guard, [this] { return proceed; });
        }
        return MockProvider::observe(event);
    }
    void wait_for_enter() {
        std::unique_lock<std::mutex> guard(mu);
        check(cv.wait_for(guard, std::chrono::seconds(5),
                          [this] { return entered; }),
              "bounded GPU completion worker never began poll");
    }
    void unblock() {
        {
            std::lock_guard<std::mutex> guard(mu);
            proceed = true;
        }
        cv.notify_all();
    }
};

static void test_completion_capacity_and_cohort() {
    auto registry = std::make_shared<Registry>(4);
    auto control = std::make_shared<Lane>(4);
    auto a = std::make_shared<BlockedPollProvider>(1, "actor-a");
    auto b = std::make_shared<MockProvider>(1, "actor-b");
    auto foreign = std::make_shared<MockProvider>(1, "actor-c", "other-tenant");
    Bridge first(registry, a, control, {"actor-a", 1}, "root", 256);
    Bridge second(registry, b, control, {"actor-b", 1}, "root", 256);
    Bridge third(registry, foreign, control, {"actor-c", 1}, "root", 256);
    (void)first.submit(1);
    (void)second.submit(1);

    // Capacity=1 counts the currently active device poll as retained.
    GpuActorCompletionExecutorV1 executor(a->binding(), 1);
    a->observation = Port::Observation::CompleteOk;
    check(executor.request(registry, first.completion_lease_id()),
          "first GPU event notification rejected");
    a->wait_for_enter();

    // No duplicate queued job: one active event plus exactly one rearm.
    check(executor.request(registry, first.completion_lease_id()),
          "active event notification was lost");
    check(!executor.request(registry, first.completion_lease_id()),
          "duplicate GPU completion rearm was not coalesced");
    const auto during = executor.stats();
    check(during.retained == 1 && during.active
          && during.queued == 0 && during.rearmed == 1,
          "GPU completion admission failed to bound active demand");

    try {
        (void)executor.request(registry, second.completion_lease_id());
        check(false, "GPU completion capacity exceeded");
    } catch (const std::logic_error& error) {
        check(std::string(error.what()) == "GPU_COMPLETION_QUEUE_CAPACITY_EXHAUSTED",
              "GPU completion capacity rejected with incorrect reason");
    }
    try {
        (void)executor.request(registry, third.completion_lease_id());
        check(false, "foreign tenant entered GPU completion queue");
    } catch (const std::logic_error& error) {
        check(std::string(error.what()) == "GPU_COMPLETION_FOREIGN_QUEUE_COHORT",
              "GPU completion accepted foreign device/tenant/queue binding");
    }

    a->unblock(); // a real device event would make the provider poll terminal
    executor.await_idle();
    const auto after = executor.stats();
    check(after.processed == 2 && after.failed == 0
          && after.retained == 0 && !after.active,
          "GPU completion rerun did not release its retained slot");
    check(a->observed_events.load() == 1
          && a->foreign_host_polls.load() == 0,
          "GPU completion poll ran twice or on a root thread");
    first.retire_gracefully();

    b->observation = Port::Observation::CompleteOk;
    check(executor.request(registry, second.completion_lease_id()),
          "GPU completion failed to reuse freed executor capacity");
    executor.await_idle();
    check(b->observed_events.load() == 1
          && b->foreign_host_polls.load() == 0,
          "independent actor completion did not run on host GPU executor");
    second.retire_gracefully();
    third.retire_gracefully();
    const auto final_stats = executor.stats();
    check(final_stats.accepted == 2 && final_stats.rearmed == 1
          && final_stats.processed == 3 && final_stats.rejected == 1
          && final_stats.retained == 0,
          "GPU completion executor accounting incorrect");
    std::puts("GPU_COMPLETION_BOUNDED_COHORT_PASS");
}

// OpenCL's native Api exposes poll_event(), not a guaranteed callback.
// A bounded worker must keep checking a submitted event without spinning a
// VM actor carrier and without automatically trying unknown-enqueue recovery.
class RecoverableUnknownProvider final : public MockProvider {
public:
    using MockProvider::MockProvider;
    std::atomic<int> reconcilations{0};
    std::atomic<bool> recovered{false};
    EnqueueResult reconcile_unknown(Turn turn) override {
        ++reconcilations;
        if (!recovered.load(std::memory_order_acquire))
            return {EnqueueState::Unknown, nullptr};
        return MockProvider::reconcile_unknown(turn);
    }
};

static void test_timed_opencl_repoll() {
    auto reg = std::make_shared<Registry>(3);
    auto lane = std::make_shared<Lane>(3);
    auto port = std::make_shared<MockProvider>(1, "actor-timed");
    Bridge bridge(reg, port, lane, {"actor-timed",1}, "root", 128);
    (void)bridge.submit(7);
    {
        GpuActorCompletionExecutorV1 executor(
            port->binding(), 2, std::chrono::milliseconds{5});
        check(executor.request(reg, bridge.completion_lease_id()),
              "timed OpenCL poll was rejected");
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{3};
        while (port->observed_events.load(std::memory_order_acquire) < 3
                && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        check(port->observed_events.load() >= 3,
              "submitted OpenCL operation never repolled on GPU host worker");
        check(bridge.committed_state() == 0,
              "repeated pending GPU polls committed actor state");
        port->observation.store(Port::Observation::CompleteOk,
                                std::memory_order_release);
        executor.await_idle();
        check(bridge.committed_state() == 7,
              "timed completion did not commit authenticated event");
        check(port->foreign_host_polls.load() == 0,
              "timed OpenCL polling occurred on root/CONTROL host thread");
        const auto st = executor.stats();
        check(st.timed_repolls >= 2 && st.retained == 0
            && st.failed == 0 && st.processed >= 3,
              "timed GPU completion polling did not settle bounded lease");
    }
    bridge.retire_gracefully();

    auto unknown = std::make_shared<RecoverableUnknownProvider>(1, "actor-unknown");
    unknown->unknown = true;
    Bridge pending(reg, unknown, lane, {"actor-unknown",1}, "root", 128);
    (void)pending.submit(9);
    GpuActorCompletionExecutorV1 executor(
        unknown->binding(), 1, std::chrono::milliseconds{3});
    check(executor.request(reg, pending.completion_lease_id()),
          "unknown-enqueue host poll not accepted");
    executor.await_idle();
    check(unknown->reconcilations.load() == 1,
          "unknown enqueue did not enter supervisor-owned reconciliation");
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    check(unknown->reconcilations.load() == 1
          && unknown->observed_events.load() == 0,
          "unknown enqueue was automatically retried without recovery proof");
    check(pending.committed_state() == 0,
          "unknown enqueue falsely committed GPU actor state");
    unknown->recovered.store(true, std::memory_order_release);
    unknown->observation.store(Port::Observation::CompleteOk,
                               std::memory_order_release);
    check(executor.request(reg, pending.completion_lease_id()),
          "recovery worker could not signal GPU completion executor");
    executor.await_idle();
    check(pending.committed_state() == 9
          && unknown->observed_events.load() == 1,
          "explicit recovery did not resume authenticated event completion");
    pending.retire_gracefully();
    std::puts("GPU_COMPLETION_TIMED_POLL_PASS");
}

static void test_pending_worker_shutdown_preserves_gpu_loan() {
    auto reg = std::make_shared<Registry>(2);
    auto lane = std::make_shared<Lane>(2);
    auto port = std::make_shared<MockProvider>(1, "actor-stop-poller");
    Bridge bridge(reg, port, lane, {"actor-stop-poller",1}, "root", 128);
    (void)bridge.submit(1);
    {
        GpuActorCompletionExecutorV1 polling(
            port->binding(), 1, std::chrono::milliseconds{4});
        check(polling.request(reg, bridge.completion_lease_id()),
              "initial pending poll failed");
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{3};
        while (port->observed_events.load() < 2
                && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        check(port->observed_events.load() >= 2,
              "polling worker did not establish a pending subscription");
    } // stop polling only, NEVER release the physical GPU device loan
    check(bridge.committed_state() == 0,
          "shutdown of GPU completion worker forged physical completion");
    port->observation.store(Port::Observation::CompleteOk,
                            std::memory_order_release);
    {
        GpuActorCompletionExecutorV1 recovery(port->binding(), 1);
        check(recovery.request(reg, bridge.completion_lease_id()),
              "replacement completion worker could not adopt host lease");
        recovery.await_idle();
        check(recovery.stats().failed == 0 && bridge.committed_state() == 1,
              "replacement GPU completion worker lost the native provider event");
    }
    bridge.retire_gracefully();
    std::puts("GPU_COMPLETION_SHUTDOWN_PRESERVES_LOAN_PASS");
}

int main() {
    test_timed_opencl_repoll();
    test_pending_worker_shutdown_preserves_gpu_loan();
    test_completion_capacity_and_cohort();
    run_case(Port::Observation::CompleteOk, false, false);
    run_case(Port::Observation::CompleteFailed, false, false);
    run_case(Port::Observation::CompleteOk, true, false);
    run_case(Port::Observation::CompleteOk, false, true);
    run_case(Port::Observation::CompleteOk, true, false, true);
    run_case(Port::Observation::CompleteOk, false, false, false, true);
    run_case(Port::Observation::CompleteOk, false, false, false, false, true);
    return 0;
}
