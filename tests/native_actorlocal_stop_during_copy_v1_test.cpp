// Host-only lifecycle regression. ActorLocal must not expose a pinned read
// after the scheduler STOP fence, even when the original carrier is still
// physically executing its admitted callback. This is not a guest .atto test.
#include "oreslang/runtime/ActorLocalSchedulerTurnV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace oreslang::runtime;
namespace {
struct PausingHostValue {
    std::atomic<bool>* entered;
    std::atomic<bool>* release;
    int payload;
    PausingHostValue(std::atomic<bool>& e, std::atomic<bool>& r, int value)
        : entered(&e), release(&r), payload(value) {}
    PausingHostValue(PausingHostValue&&) noexcept = default;
    PausingHostValue(const PausingHostValue& other)
        : entered(other.entered), release(other.release),
          payload(other.payload) {
        entered->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire))
            std::this_thread::yield();
    }
};
struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actors[2]{};
    ActorLocalScopeV1::Ptr scopes[2];
    ExecutionStampV1 stamps[2]{};
    ActorLocalKeyV1<PausingHostValue> stopped_read;
    ActorLocalKeyV1<int> sibling_value;
    std::atomic<bool> copying{false};
    std::atomic<bool> release{false};
    std::mutex mutex;
    std::condition_variable changed;
    int completed[2]{};
};
void on_message(void* context, std::size_t index, std::uint64_t payload) {
    auto& s = *static_cast<Scenario*>(context);
    assert(index < 2);
    assert(actor_local_current_turn_v1(
        s.scheduler, s.actors[index], s.stamps[index]));
    if (index == 0) {
        assert(payload == 1);
        assert(actor_local_set_on_turn_v1(
            s.scheduler, s.actors[0], *s.scopes[0], s.stamps[0],
            s.stopped_read, PausingHostValue(s.copying, s.release, 7)));
        // The copy constructor pauses this actor without retaining the
        // store mutex. The CONTROL thread STOPs the actor before release.
        const auto snapshot = actor_local_get_on_turn_v1(
            s.scheduler, s.actors[0], *s.scopes[0],
            s.stamps[0], s.stopped_read);
        assert(!snapshot);
        assert(!actor_local_current_turn_v1(
            s.scheduler, s.actors[0], s.stamps[0]));
        assert(!actor_local_set_on_turn_v1(
            s.scheduler, s.actors[0], *s.scopes[0], s.stamps[0],
            s.stopped_read, PausingHostValue(s.copying, s.release, 8)));
        assert(!actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[0], *s.scopes[0],
            s.stamps[0], s.stopped_read));
    } else {
        assert(payload == 1 || payload == 2);
        if (payload == 1) {
            assert(actor_local_set_on_turn_v1(
                s.scheduler, s.actors[1], *s.scopes[1],
                s.stamps[1], s.sibling_value, 42));
        } else {
            assert(actor_local_get_on_turn_v1(
                s.scheduler, s.actors[1], *s.scopes[1],
                s.stamps[1], s.sibling_value) == 42);
            assert(actor_local_set_on_turn_v1(
                s.scheduler, s.actors[1], *s.scopes[1],
                s.stamps[1], s.sibling_value, 43));
        }
        assert(actor_local_get_on_turn_v1(
            s.scheduler, s.actors[1], *s.scopes[1],
            s.stamps[1], s.sibling_value) == (payload == 1 ? 42 : 43));
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        assert(++s.completed[index] == static_cast<int>(payload));
    }
    s.changed.notify_all();
}
void wait_for_count(Scenario& s, std::size_t index, int count) {
    std::unique_lock<std::mutex> lock(s.mutex);
    assert(s.changed.wait_for(lock, std::chrono::seconds(8), [&] {
        return s.completed[index] >= count;
    }));
}
}
int main() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open(2, 8, 2, on_message, &s);
    assert(s.scheduler);
    for (std::size_t i = 0; i != 2; ++i) {
        s.actors[i] = ores_native_actor_scheduler_spawn(s.scheduler, i);
        assert(s.actors[i].scheduler_domain);
        s.stamps[i] = {9, 18, s.actors[i].scheduler_domain,
                       s.actors[i].actor_slot, s.actors[i].generation, 1, 1};
        s.scopes[i] = ActorLocalScopeV1::open(s.stamps[i]);
        assert(s.scopes[i]);
    }
    assert(ores_native_actor_scheduler_try_send(s.scheduler, s.actors[0], 1)
           == ORES_NATIVE_SEND_ACCEPTED);
    assert(ores_native_actor_scheduler_try_send(s.scheduler, s.actors[1], 1)
           == ORES_NATIVE_SEND_ACCEPTED);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    while (!s.copying.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(s.copying.load(std::memory_order_acquire));
    // STOP wins after the read pinned its cell but before it can return.
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[0]));
    assert(ores_native_actor_scheduler_poll_ready(s.scheduler, s.actors[1]));
    s.release.store(true, std::memory_order_release);
    wait_for_count(s, 0, 1);
    wait_for_count(s, 1, 1);
    assert(ores_native_actor_scheduler_try_send(s.scheduler, s.actors[0], 2)
           != ORES_NATIVE_SEND_ACCEPTED);
    assert(ores_native_actor_scheduler_try_send(s.scheduler, s.actors[1], 2)
           == ORES_NATIVE_SEND_ACCEPTED);
    wait_for_count(s, 1, 2);
    // Stopped scope cannot be reclaimed before its real physical callback
    // returns. The CONTROL-plane lifecycle helper owns this exact barrier.
    assert(actor_local_close_after_terminal_v1(
        s.scheduler, s.actors[0], *s.scopes[0], s.stamps[0]));
    assert(!s.scopes[0]->is_open());
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[1]));
    assert(actor_local_close_after_terminal_v1(
        s.scheduler, s.actors[1], *s.scopes[1], s.stamps[1]));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    std::puts("ACTOR_LOCAL_STOP_DURING_READ_SIBLING_SURVIVES_HOST_PASS");
}
