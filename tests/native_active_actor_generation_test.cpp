#include "oreslang/runtime/NativeActorScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
struct Observed {
    OresNativeActorScheduler* scheduler = nullptr;
    std::atomic<std::uint64_t> expected{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> future_count{0};
    std::atomic<std::uint64_t> panic_count{0};
    std::atomic<bool> bad{false};
};
static void data(void* raw, OresNativeActorRef identity, std::uint64_t payload) {
    auto* obs = static_cast<Observed*>(raw);
    if (identity.scheduler_domain == 0 || identity.actor_slot != 0
            || identity.generation != payload
            || identity.generation != obs->expected.load())
        obs->bad.store(true);
    if (identity.generation > 1) {
        const OresNativeActorRef stale{
            identity.scheduler_domain, identity.actor_slot,
            identity.generation - 1
        };
        if (ores_native_actor_scheduler_send(obs->scheduler, stale, payload)
                || ores_native_actor_scheduler_stop(obs->scheduler, stale)
                || ores_native_actor_scheduler_poll_ready(obs->scheduler, stale))
            obs->bad.store(true);
    }
    ++obs->completed;
}
static void future(void* raw, OresNativeActorRef parent, std::uint64_t) {
    auto* obs = static_cast<Observed*>(raw);
    if (!parent.scheduler_domain || parent.generation != 1)
        obs->bad.store(true);
    ++obs->future_count;
}
static void panic(void* raw, OresNativeActorRef parent,
                  OresNativeActorTerminalFailure failure) {
    auto* obs = static_cast<Observed*>(raw);
    if (!parent.scheduler_domain || parent.actor_slot != 1
            || parent.generation != 1
            || failure.child.actor_slot != 0
            || failure.child.generation != 1)
        obs->bad.store(true);
    ++obs->panic_count;
}
static bool valid(OresNativeActorRef ref) {
    return ref.scheduler_domain && ref.generation;
}
static void close_scheduler(OresNativeActorScheduler* scheduler) {
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler)
              && ores_native_actor_scheduler_try_destroy(scheduler),
          "scheduler close failed");
}
int main() {
    Observed state;
    auto* scheduler = ores_native_actor_scheduler_open_identity_control(
        1, 4, 2, data, future, panic, &state);
    check(scheduler != nullptr, "identity-mode scheduler unavailable");
    state.scheduler = scheduler;
    auto current = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(valid(current), "initial actor spawn failed");

    constexpr std::uint64_t rounds = 10000;
    for (std::uint64_t i = 0; i < rounds; ++i) {
        check(current.generation == i + 1,
              "physical generation was not monotonic");
        state.expected.store(current.generation);
        check(ores_native_actor_scheduler_send(
                  scheduler, current, current.generation),
              "current generation could not accept a typed message");
        check(ores_native_actor_scheduler_stop(scheduler, current)
                  && ores_native_actor_scheduler_wait_done(scheduler, current),
              "current actor never reached physical DONE");
        check(!state.bad.load()
                  && state.completed.load() == i + 1,
              "callback captured wrong incarnation or lost accepted work");
        check(ores_native_actor_scheduler_seal_normal_exit(scheduler, current),
              "normal physical DONE could not be classified once");
        check(!ores_native_actor_scheduler_seal_normal_exit(scheduler, current),
              "normal DONE receipt could be replayed");
        const auto next = ores_native_actor_scheduler_recycle_normal_done(
            scheduler, current);
        check(valid(next) && next.scheduler_domain == current.scheduler_domain
                  && next.actor_slot == current.actor_slot
                  && next.generation == current.generation + 1,
              "sealed active actor was not reincarnated exactly once");
        check(!ores_native_actor_scheduler_recycle_normal_done(scheduler, current)
                  .scheduler_domain
                  && !ores_native_actor_scheduler_send(scheduler, current, 1)
                  && !ores_native_actor_scheduler_poll_done(scheduler, current)
                  && !ores_native_actor_scheduler_submit(scheduler, 0, 1),
              "old-generation message or lifecycle authority crossed restart");
        check(!ores_native_actor_scheduler_poll_published(scheduler, next)
                  && !ores_native_actor_scheduler_send(scheduler, next, 1),
              "recycled active actor was published without logical admission");
        OresNativeRestartLease lease{};
        check(ores_native_actor_scheduler_acquire_restart_lease(
                  scheduler, next, &lease),
              "recycled actor lacked generation-bound lease");
        check(ores_native_actor_scheduler_publish_restart_lease(
                  scheduler, lease)
                  && ores_native_actor_scheduler_release_restart_lease(
                      scheduler, lease),
              "recycled actor publication failed");
        current = next;
    }
    check(!state.bad.load() && state.completed.load() == rounds,
          "identity callbacks delivered duplicate or stale generations");
    check(ores_native_actor_scheduler_stop(scheduler, current)
              && ores_native_actor_scheduler_wait_done(scheduler, current),
          "final new generation physical STOP failed");
    close_scheduler(scheduler);

    // Typed CONTROL and terminal PANIC must carry the parent incarnation
    // as well as the original typed child identity. A failed generation is
    // never reusable under the normal-exit fast path.
    Observed ctrl;
    auto* typed = ores_native_actor_scheduler_open_identity_control(
        2, 2, 2, data, future, panic, &ctrl);
    check(typed != nullptr, "typed identity scheduler open failed");
    ctrl.scheduler = typed;
    const auto child = ores_native_actor_scheduler_spawn(typed, 0);
    const auto parent = ores_native_actor_scheduler_spawn(typed, 1);
    check(valid(child) && valid(parent), "typed supervisor actors did not spawn");
    check(ores_native_actor_scheduler_publish_control(typed, parent, 33),
          "typed CONTROL rejected");
    // The native CONTROL lane has one reserved slot. A second typed PANIC
    // cannot be admitted before the earlier future callback drains; do
    // not confuse intentional backpressure with generation ABI failure.
    const auto control_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ctrl.future_count.load() != 1
            && std::chrono::steady_clock::now() < control_deadline)
        std::this_thread::sleep_for(1ms);
    check(ctrl.future_count.load() == 1,
          "typed CONTROL callback did not finish before PANIC admission");
    check(ores_native_actor_scheduler_stop(typed, child)
              && ores_native_actor_scheduler_wait_done(typed, child),
          "PANIC child did not become physically DONE");
    const OresNativeActorTerminalFailure terminal{
        ORES_NATIVE_CONTROL_PANIC, child, 79, 89
    };
    check(ores_native_actor_scheduler_publish_failure(typed, parent, terminal),
          "typed generation-scoped PANIC was not admitted");
    check(ores_native_actor_scheduler_stop(typed, parent)
              && ores_native_actor_scheduler_wait_done(typed, parent),
          "typed parent CONTROL did not drain");
    check(ctrl.future_count.load() == 1 && ctrl.panic_count.load() == 1
              && !ctrl.bad.load(), "typed CONTROL/PANIC lost actor generation");
    check(!ores_native_actor_scheduler_seal_normal_exit(typed, child)
              && !ores_native_actor_scheduler_recycle_normal_done(typed, child)
                  .scheduler_domain,
          "unreconciled PANIC was recycled as ordinary normal exit");
    close_scheduler(typed);

    // A legacy callback cannot provide generation binding; fail closed
    // even when the actor has successfully completed and sealed.
    auto* legacy = ores_native_actor_scheduler_open(
        1, 1, 1, [](void*, size_t, std::uint64_t) {}, nullptr);
    check(legacy != nullptr, "legacy scheduler unavailable");
    const auto old = ores_native_actor_scheduler_spawn(legacy, 0);
    check(ores_native_actor_scheduler_stop(legacy, old)
              && ores_native_actor_scheduler_wait_done(legacy, old)
              && ores_native_actor_scheduler_seal_normal_exit(legacy, old)
              && !ores_native_actor_scheduler_recycle_normal_done(legacy, old)
                  .scheduler_domain,
          "legacy slot-only handler bypassed generation-aware recycling");
    close_scheduler(legacy);
    std::puts("NATIVE_ACTIVE_GENERATION_PASS cycles=10000 stale=1 typed_control=1 panic_fenced=1 legacy_fenced=1");
}
