#include "oreslang/runtime/ActorLocalSchedulerTurnV1.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

using namespace oreslang::runtime;

namespace {
struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actors[2]{};
    ActorLocalScopeV1::Ptr scope[2];
    ExecutionStampV1 stamps[2]{};
    LegacyActorLocalKeyV1<int> key;
    std::mutex mutex;
    std::condition_variable done;
    int passes[2]{};
};

void on_actor_message(void* user, std::size_t index, std::uint64_t payload) {
    auto& s = *static_cast<Scenario*>(user);
    assert(index < 2 && (payload == 1 || payload == 2));
    assert(actor_local_current_turn_v1(
        s.scheduler, s.actors[index], s.stamps[index]));
    assert(!actor_local_current_turn_v1(
        s.scheduler, s.actors[1-index], s.stamps[1-index]));

    // Retirement must never park this actor's carrier while its callback
    // remains active, even when the caller has an otherwise correct stamp.
    assert(!actor_local_close_after_terminal_v1(
        s.scheduler, s.actors[index], *s.scope[index], s.stamps[index]));
    if (payload == 1) {
        assert(actor_local_set_on_turn_v1(s.scheduler, s.actors[index],
            *s.scope[index], s.stamps[index], s.key,
            static_cast<int>(100 + index)));
        assert(!actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[1-index],
            s.stamps[1-index], s.key));
        auto wrong_tenant = s.stamps[index];
        ++wrong_tenant.tenant;
        assert(!actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            wrong_tenant, s.key));
        auto wrong_isolate = s.stamps[index];
        ++wrong_isolate.isolate;
        assert(!actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            wrong_isolate, s.key));
        assert(actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            s.stamps[index], s.key));
        assert(!actor_local_erase_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            s.stamps[index], s.key));
        assert(!actor_local_get_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            s.stamps[index], s.key));
        assert(actor_local_set_on_turn_v1(s.scheduler, s.actors[index],
            *s.scope[index], s.stamps[index], s.key,
            static_cast<int>(100 + index)));
    } else {
        assert(actor_local_get_on_turn_v1(s.scheduler, s.actors[index],
            *s.scope[index], s.stamps[index], s.key) ==
            static_cast<int>(100 + index));
        assert(!actor_local_get_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[1-index],
            s.stamps[1-index], s.key));
        auto stale = s.stamps[index];
        ++stale.actor_generation;
        assert(!actor_local_get_on_turn_v1(
            s.scheduler, s.actors[index], *s.scope[index],
            stale, s.key));
    }

    {
        std::lock_guard<std::mutex> guard(s.mutex);
        assert(++s.passes[index] == static_cast<int>(payload));
    }
    s.done.notify_one();
}

void round(Scenario& s, int round_number) {
    for (auto& actor : s.actors)
        assert(ores_native_actor_scheduler_try_send(
            s.scheduler, actor, static_cast<std::uint64_t>(round_number))
            == ORES_NATIVE_SEND_ACCEPTED);
    std::unique_lock<std::mutex> lock(s.mutex);
    assert(s.done.wait_for(lock, std::chrono::seconds(8), [&] {
        return s.passes[0] >= round_number &&
               s.passes[1] >= round_number;
    }));
}
} // namespace

int main() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open(
        2, 8, 2, on_actor_message, &s);
    assert(s.scheduler);
    for (std::size_t i = 0; i < 2; ++i) {
        s.actors[i] = ores_native_actor_scheduler_spawn(s.scheduler, i);
        assert(s.actors[i].scheduler_domain);
        s.stamps[i] = {
            9, 18, s.actors[i].scheduler_domain,
            s.actors[i].actor_slot, s.actors[i].generation, 1, 1
        };
        s.scope[i] = ActorLocalScopeV1::open(s.stamps[i]);
        assert(s.scope[i]);
    }
    // Same actor/stamp observed on a foreign pthread is never privileged.
    assert(!actor_local_current_turn_v1(
        s.scheduler, s.actors[0], s.stamps[0]));
    assert(!actor_local_set_on_turn_v1(
        s.scheduler, s.actors[0], *s.scope[0],
        s.stamps[0], s.key, 999));
    assert(!actor_local_erase_on_turn_v1(
        s.scheduler, s.actors[0], *s.scope[0],
        s.stamps[0], s.key));

    round(s, 1);
    round(s, 2);

    // Terminal-fenced teardown must not reclaim storage for a still-live
    // actor, even from an authorized CONTROL-plane host OS thread.
    for (std::size_t i = 0; i < 2; ++i) {
        assert(!actor_local_close_after_terminal_v1(
            s.scheduler, s.actors[i], *s.scope[i], s.stamps[i]));
        assert(s.scope[i]->is_open());
    }

    for (std::size_t i = 0; i < 2; ++i) {
        assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[i]));
        auto wrong_owner = s.stamps[i];
        ++wrong_owner.actor_generation;
        assert(!actor_local_close_after_terminal_v1(
            s.scheduler, s.actors[i], *s.scope[i], wrong_owner));
        // The lifecycle helper itself executes the real terminal fence.
        assert(actor_local_close_after_terminal_v1(
            s.scheduler, s.actors[i], *s.scope[i], s.stamps[i]));
        assert(!s.scope[i]->is_open());
        assert(!s.scope[i]->get_copy(s.stamps[i], s.key));
        assert(!actor_local_close_after_terminal_v1(
            s.scheduler, s.actors[i], *s.scope[i], s.stamps[i]));
    }
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    std::puts("ACTOR_LOCAL_SCHEDULER_TURN_HOST_PASS");
}
