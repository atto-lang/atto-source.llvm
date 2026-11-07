// HOST-only negative regression: an actor carrier from scheduler A must
// never synchronously wait for terminal retirement in scheduler B, even when
// B is already terminal and its logical stamp/scope are otherwise valid.
#include "oreslang/runtime/ActorLocalSchedulerTurnV1.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

using namespace oreslang::runtime;
namespace {
struct State {
    OresNativeActorScheduler* source = nullptr;
    OresNativeActorScheduler* target = nullptr;
    OresNativeActorRef target_actor{};
    ActorLocalScopeV1::Ptr target_scope;
    ExecutionStampV1 target_stamp{};
    ActorLocalKeyV1<int> key;
    std::mutex mutex;
    std::condition_variable done_cv;
    bool attempted = false;
};
void target_callback(void*, std::size_t, std::uint64_t) {
    // No target message is sent; this function must remain unreachable.
    assert(false && "target actor unexpectedly executed user code");
}
void source_callback(void* raw, std::size_t index, std::uint64_t payload) {
    auto& s = *static_cast<State*>(raw);
    assert(index == 0 && payload == 1);
    assert(ores_native_actor_scheduler_is_any_current_carrier_v1());
    assert(ores_native_actor_scheduler_is_current_carrier(s.source));
    assert(!ores_native_actor_scheduler_is_current_carrier(s.target));
    // Prior adapter checked only target scheduler carrier ownership, so a
    // foreign carrier could wait/close this already terminal target scope.
    assert(!actor_local_close_after_terminal_v1(
        s.target, s.target_actor, *s.target_scope, s.target_stamp));
    assert(s.target_scope->is_open());
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.attempted = true;
    }
    s.done_cv.notify_one();
}
}
int main() {
    State s;
    s.target = ores_native_actor_scheduler_open(1, 4, 1, target_callback, &s);
    assert(s.target);
    s.target_actor = ores_native_actor_scheduler_spawn(s.target, 0);
    assert(s.target_actor.scheduler_domain);
    s.target_stamp = {9, 18, s.target_actor.scheduler_domain,
                      s.target_actor.actor_slot, s.target_actor.generation, 1, 1};
    s.target_scope = ActorLocalScopeV1::open(s.target_stamp);
    assert(s.target_scope);
    assert(s.target_scope->set(s.target_stamp, s.key, 42));
    assert(ores_native_actor_scheduler_stop(s.target, s.target_actor));
    assert(ores_native_actor_scheduler_wait_done(s.target, s.target_actor));
    assert(ores_native_actor_scheduler_poll_done(s.target, s.target_actor));

    s.source = ores_native_actor_scheduler_open(1, 4, 1, source_callback, &s);
    assert(s.source);
    const auto caller = ores_native_actor_scheduler_spawn(s.source, 0);
    assert(caller.scheduler_domain);
    assert(ores_native_actor_scheduler_try_send(s.source, caller, 1)
           == ORES_NATIVE_SEND_ACCEPTED);
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        assert(s.done_cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return s.attempted;
        }));
    }

    // A real external CONTROL thread (not a native carrier) may now perform
    // the generation-fenced, independently checked close.
    assert(!ores_native_actor_scheduler_is_any_current_carrier_v1());
    assert(s.target_scope->is_open());
    assert(s.target_scope->get_copy(s.target_stamp, s.key) == 42);
    assert(actor_local_close_after_terminal_v1(
        s.target, s.target_actor, *s.target_scope, s.target_stamp));
    assert(!s.target_scope->is_open());
    assert(!actor_local_close_after_terminal_v1(
        s.target, s.target_actor, *s.target_scope, s.target_stamp));

    assert(ores_native_actor_scheduler_stop(s.source, caller));
    ores_native_actor_scheduler_retire(s.source);
    assert(ores_native_actor_scheduler_drain(s.source));
    assert(ores_native_actor_scheduler_try_destroy(s.source));
    ores_native_actor_scheduler_retire(s.target);
    assert(ores_native_actor_scheduler_drain(s.target));
    assert(ores_native_actor_scheduler_try_destroy(s.target));
    std::puts("ACTOR_LOCAL_CROSS_SCHEDULER_CARRIER_RETIRE_DENIED_HOST_PASS");
}
