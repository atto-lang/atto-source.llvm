#define ORESLANG_RW_INTERNAL_TEST_HOOKS 1
#include "oreslang/runtime/ActorStateReadWriteSchedulerBridge.hpp"

#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

using Registry = oreslang::runtime::ActorStateReadWriteContinuations;
using Bridge = oreslang::runtime::ActorStateReadWriteSchedulerBridge;
using Mode = Registry::Mode;
using Status = Registry::Status;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

struct State {
    Registry registry;
    Bridge* bridge = nullptr;
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    std::atomic<int> accepted{0}, rejected{0}, failures{0};
    std::atomic<int> data_rejected{0}, cancelled{0}, unwind_released{0};
};
extern "C" void on_data(void* raw, size_t actor, uint64_t payload) {
    auto& s = *static_cast<State*>(raw);
    if (actor != s.actor.actor_slot || payload != 41
            || !ores_native_actor_scheduler_is_current_actor(s.scheduler, s.actor)) {
        s.failures.fetch_add(1);
        return;
    }
    // Same physical actor lane but WRONG callback kind: cannot claim a
    // still-pending READY ticket or finalize it through a DATA callback.
    const OresNativeActorStateWake forged{
        s.registry.incarnation(), 1, ORES_NATIVE_RW_READY
    };
    if (ores_native_actor_scheduler_is_current_state_wake(
            s.scheduler, s.actor, forged)
            || s.bridge->try_enter(forged, Mode::Read))
        s.failures.fetch_add(1);
    bool denied = false;
    try { s.bridge->complete(1); }
    catch (const std::logic_error&) { denied = true; }
    if (!denied) s.failures.fetch_add(1);
    s.data_rejected.fetch_add(1);
}
extern "C" void on_state(void* raw, size_t actor, OresNativeActorStateWake wake) {
    auto& s = *static_cast<State*>(raw);
    if (actor != s.actor.actor_slot
            || !ores_native_actor_scheduler_is_current_actor(s.scheduler, s.actor)) {
        s.failures.fetch_add(1);
        return;
    }
    if (!ores_native_actor_scheduler_is_current_state_wake(
            s.scheduler, s.actor, wake))
        s.failures.fetch_add(1);
    const OresNativeActorStateWake mismatched{
        wake.incarnation, wake.continuation_id + 10, wake.status
    };
    if (ores_native_actor_scheduler_is_current_state_wake(
            s.scheduler, s.actor, mismatched)
            || s.bridge->try_enter(mismatched, Mode::Read))
        s.failures.fetch_add(1);
    if (wake.status == ORES_NATIVE_RW_CANCELLED) {
        if (!s.bridge->is_own_cancellation(wake))
            s.failures.fetch_add(1);
        s.cancelled.fetch_add(1);
        return;
    }
    const OresNativeActorStateWake false_cancellation{
        wake.incarnation, wake.continuation_id, ORES_NATIVE_RW_CANCELLED
    };
    if (s.bridge->is_own_cancellation(false_cancellation))
        s.failures.fetch_add(1);
    if (wake.continuation_id == 5) {
        // A guest throw is contained by this trusted C++ callback wrapper.
        // Even without explicit finish(), the scoped grant must be released
        // before returning across the native C scheduler callback boundary.
        try {
            Bridge::ScopedWake scope(*s.bridge, wake, Mode::Read);
            if (!scope) {
                s.failures.fetch_add(1);
                return;
            }
            throw std::runtime_error("simulated guest failure");
        } catch (const std::runtime_error&) {
            s.unwind_released.fetch_add(1);
        }
        return;
    }
    if (!s.bridge->try_enter(wake, Mode::Read)) {
        s.rejected.fetch_add(1);
        return;
    }
    // A second trusted host component cannot use the older completion
    // entrypoint to release this already-dispatched grant while the actor
    // callback is still running. Only bridge.complete() owns that release.
    bool legacy_completion_denied = false;
    try { s.registry.complete_undispatched_for_test(wake.incarnation, wake.continuation_id); }
    catch (const std::logic_error&) { legacy_completion_denied = true; }
    if (!legacy_completion_denied ||
            !s.registry.admitted(wake.incarnation, wake.continuation_id, Mode::Read)) {
        s.failures.fetch_add(1);
        return;
    }
    // Retirement cannot revoke a dispatched loan while the actor turn is
    // running. It must reject without changing ticket or wake state.
    bool retirement_denied = false;
    try { s.registry.retire(); }
    catch (const std::logic_error&) { retirement_denied = true; }
    if (!retirement_denied ||
            !s.registry.admitted(wake.incarnation, wake.continuation_id, Mode::Read)) {
        s.failures.fetch_add(1);
        return;
    }
    // In particular, an immediately admitted read ticket must *not*
    // execute on the host producer thread; it is owned by this actor lane.
    s.accepted.fetch_add(1);
    try {
        s.bridge->complete(wake.continuation_id);
    } catch (...) {
        s.failures.fetch_add(1);
    }
}
int main() {
    State state;
    state.scheduler = ores_native_actor_scheduler_open_state_wakes(
        1, 4, 2, on_data, nullptr, nullptr, on_state, &state);
    CHECK(state.scheduler != nullptr);
    state.actor = ores_native_actor_scheduler_spawn(state.scheduler, 0);
    CHECK(state.actor.scheduler_domain != 0);
    Bridge bridge(state.registry, state.scheduler, state.actor);
    state.bridge = &bridge;
    const auto incarnation = state.registry.incarnation();

    // Force receiver-before-ack race: publish directly, without pumping.
    // The actor callback must be able to atomically claim the valid READY.
    CHECK(state.registry.request(incarnation, 1, Mode::Read).status == Status::Ready);
    CHECK(!state.registry.admitted(incarnation, 1, Mode::Read));
    CHECK(ores_native_actor_scheduler_send(state.scheduler, state.actor, 41));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.data_rejected.load() == 1);
    CHECK(state.failures.load() == 0);
    CHECK(!state.registry.admitted(incarnation, 1, Mode::Read));
    const OresNativeActorStateWake first{incarnation, 1, ORES_NATIVE_RW_READY};
    CHECK(ores_native_actor_scheduler_publish_state_wake(
        state.scheduler, state.actor, first));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.accepted.load() == 1);
    CHECK(state.registry.pending_notifications().empty());

    // Normal producer pump calls the new scheduler admission API.
    CHECK(state.registry.request(incarnation, 2, Mode::Read).status == Status::Ready);
    CHECK(bridge.pump(1) == 1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.accepted.load() == 2);
    CHECK(state.registry.pending_notifications().empty());

    // Replay and actor-incarnation forgery reach no guest callback.
    CHECK(ores_native_actor_scheduler_publish_state_wake(
        state.scheduler, state.actor, first));
    const OresNativeActorStateWake forged{incarnation + 1, 2, ORES_NATIVE_RW_READY};
    CHECK(ores_native_actor_scheduler_publish_state_wake(
        state.scheduler, state.actor, forged) == 0 ||
          ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.accepted.load() == 2);
    CHECK(state.rejected.load() >= 1);
    CHECK(state.failures.load() == 0);

    // Another READY notification cannot be claimed by a host caller.
    CHECK(state.registry.request(incarnation, 3, Mode::Read).status == Status::Ready);
    const OresNativeActorStateWake third{incarnation, 3, ORES_NATIVE_RW_READY};
    CHECK(!bridge.try_enter(third, Mode::Read));
    CHECK(bridge.pump(1) == 1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.accepted.load() == 3);
    CHECK(state.failures.load() == 0);

    // Exact CANCELLED notifications authorize terminal bookkeeping only
    // from that callback, never a READY claim.
    CHECK(state.registry.request(incarnation, 4, Mode::Write).status == Status::Ready);
    state.registry.cancel_pending(incarnation, 4);
    CHECK(bridge.pump(1) == 1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.cancelled.load() == 1);
    CHECK(state.accepted.load() == 3);
    CHECK(state.failures.load() == 0);

    // Native guest failure must not strand the RW grant or a queued writer.
    CHECK(state.registry.request(incarnation, 5, Mode::Read).status == Status::Ready);
    CHECK(bridge.pump(1) == 1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.unwind_released.load() == 1);
    CHECK(state.registry.pending_notifications().empty());
    CHECK(state.failures.load() == 0);
    CHECK(state.registry.request(incarnation, 6, Mode::Write).status == Status::Ready);
    state.registry.cancel_pending(incarnation, 6);
    CHECK(bridge.pump(1) == 1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    CHECK(state.cancelled.load() == 2);
    CHECK(state.failures.load() == 0);

    CHECK(ores_native_actor_scheduler_stop(state.scheduler, state.actor));
    state.registry.retire();
    ores_native_actor_scheduler_retire(state.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(state.scheduler));
    return 0;
}
