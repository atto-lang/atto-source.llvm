#include "oreslang/runtime/ActorStateReadWriteSchedulerBridge.hpp"

#include <atomic>
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
};
extern "C" void on_data(void*, size_t, uint64_t) {
    std::abort();
}
extern "C" void on_state(void* raw, size_t actor, OresNativeActorStateWake wake) {
    auto& s = *static_cast<State*>(raw);
    if (actor != s.actor.actor_slot
            || !ores_native_actor_scheduler_is_current_actor(s.scheduler, s.actor)) {
        s.failures.fetch_add(1);
        return;
    }
    if (!s.bridge->try_enter(wake, Mode::Read)) {
        s.rejected.fetch_add(1);
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

    CHECK(ores_native_actor_scheduler_stop(state.scheduler, state.actor));
    state.registry.retire();
    ores_native_actor_scheduler_retire(state.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(state.scheduler));
    return 0;
}
