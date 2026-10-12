#include "oreslang/runtime/NativeActorScheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
static void no_op(void*, size_t, std::uint64_t) {}
static void no_panic(void*, size_t, OresNativeActorTerminalFailure) {}

static void shutdown(OresNativeActorScheduler* scheduler) {
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler)
          && ores_native_actor_scheduler_try_destroy(scheduler),
          "native scheduler failed to drain and destroy");
}

int main() {
    // A candidate is real/ready but cannot receive *any* normal messages
    // until its supervisor's registry and routing indices have committed.
    for (int mode = 0; mode < 3; ++mode) {
        auto* scheduler = ores_native_actor_scheduler_open(2, 4, 2, no_op, nullptr);
        check(scheduler != nullptr, "scheduler creation failed");
        auto parent = ores_native_actor_scheduler_spawn(scheduler, 0);
        auto candidate = ores_native_actor_scheduler_spawn_unpublished(scheduler, 1);
        check(parent.scheduler_domain && candidate.scheduler_domain,
              "scheduler actor spawn failed");
        check(ores_native_actor_scheduler_poll_ready(scheduler, candidate),
              "physically spawned candidate should be ready");
        check(!ores_native_actor_scheduler_send(scheduler, candidate, 1)
                  && !ores_native_actor_scheduler_submit(scheduler, 1, 1),
              "unpublished physical incarnation accepted normal traffic");
        OresNativeRestartLease lease{};
        check(ores_native_actor_scheduler_acquire_restart_lease(
                  scheduler, candidate, &lease) && lease.nonce,
              "scheduler failed to issue a guarded lease");
        OresNativeRestartLease recursive{};
        check(!ores_native_actor_scheduler_acquire_restart_lease(
                  scheduler, candidate, &recursive),
              "same-thread nested lease acquisition must reject, not deadlock");
        check(!ores_native_actor_scheduler_stop(scheduler, candidate),
              "same-thread physical STOP during a lease must reject");
        check(!ores_native_actor_scheduler_try_destroy(scheduler),
              "same-thread destroy during a lease must reject");
        OresNativeRestartLease forged = lease;
        ++forged.nonce;
        check(!ores_native_actor_scheduler_publish_restart_lease(scheduler, forged)
                  && !ores_native_actor_scheduler_release_restart_lease(scheduler, forged),
              "forged admission lease was accepted");
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        std::thread stopper([&] {
            started.store(true);
            if (mode == 1)
                ores_native_actor_scheduler_retire(scheduler);
            else
                check(ores_native_actor_scheduler_stop(scheduler, candidate),
                      "concurrent physical stop failed");
            finished.store(true);
        });
        while (!started.load()) std::this_thread::yield();
        std::this_thread::sleep_for(15ms);
        const bool stayed_ready = ores_native_actor_scheduler_poll_ready(
            scheduler, candidate);
        const bool stop_waited = !finished.load();
        bool published = false;
        if (mode != 2) {
            published = ores_native_actor_scheduler_publish_restart_lease(
                scheduler, lease);
        }
        const bool released = ores_native_actor_scheduler_release_restart_lease(
            scheduler, lease);
        stopper.join();
        check(stayed_ready && stop_waited,
              "physical retirement invalidated a live admission lease");
        check(released, "scheduler refused original lease release");
        check(mode == 2 || published,
              "scheduler rejected valid publication while STOP/retire waited");
        check(!ores_native_actor_scheduler_poll_ready(scheduler, candidate)
                  && !ores_native_actor_scheduler_send(scheduler, candidate, 2),
              "retired incarnation accepted messages after lease release");
        if (mode != 1) shutdown(scheduler);
        else check(ores_native_actor_scheduler_drain(scheduler)
                        && ores_native_actor_scheduler_try_destroy(scheduler),
                   "global scheduler retirement failed to drain");
    }
    // A published replacement is never admissible for a second restart
    // transaction, even when it retains a valid physical actor reference.
    auto* scheduler = ores_native_actor_scheduler_open(1, 2, 1, no_op, nullptr);
    check(scheduler != nullptr, "second scheduler creation failed");
    const auto candidate = ores_native_actor_scheduler_spawn_unpublished(scheduler, 0);
    OresNativeRestartLease first{};
    check(ores_native_actor_scheduler_acquire_restart_lease(scheduler, candidate, &first),
          "first admission lease missing");
    check(ores_native_actor_scheduler_publish_restart_lease(scheduler, first)
              && ores_native_actor_scheduler_release_restart_lease(scheduler, first),
          "first admission publication failed");
    OresNativeRestartLease replay{};
    check(!ores_native_actor_scheduler_acquire_restart_lease(scheduler, candidate, &replay)
              && ores_native_actor_scheduler_send(scheduler, candidate, 99),
          "published incarnation could be re-admitted");
    shutdown(scheduler);
    // A thread-affine token may not be consumed from another thread, even
    // when its nonce and physical generation are otherwise exact.
    scheduler = ores_native_actor_scheduler_open(1, 2, 1, no_op, nullptr);
    check(scheduler != nullptr, "foreign-thread scheduler creation failed");
    const auto hidden = ores_native_actor_scheduler_spawn_unpublished(scheduler, 0);
    OresNativeRestartLease owned{};
    check(ores_native_actor_scheduler_acquire_restart_lease(scheduler, hidden, &owned),
          "thread-affine lease acquisition failed");
    std::atomic<bool> foreign_rejected{false};
    std::thread foreign([&] {
        foreign_rejected.store(
            !ores_native_actor_scheduler_publish_restart_lease(scheduler, owned)
            && !ores_native_actor_scheduler_release_restart_lease(scheduler, owned));
    });
    foreign.join();
    check(foreign_rejected.load(),
          "foreign host thread used another thread's scheduler lease");
    check(ores_native_actor_scheduler_release_restart_lease(scheduler, owned),
          "original owner could not release thread-affine lease");
    shutdown(scheduler);
    // Hidden actor incarnations must not be usable as a CONTROL destination
    // or fabricate a terminal PANIC about an incarnation never published.
    auto* typed = ores_native_actor_scheduler_open_typed_control(
        3, 2, 2, no_op, no_op, no_panic, nullptr);
    check(typed != nullptr, "typed CONTROL scheduler failed to open");
    const auto control_parent = ores_native_actor_scheduler_spawn(typed, 0);
    const auto hidden_child = ores_native_actor_scheduler_spawn_unpublished(typed, 1);
    const auto hidden_parent = ores_native_actor_scheduler_spawn_unpublished(typed, 2);
    check(!ores_native_actor_scheduler_publish_control(typed, hidden_parent, 3),
          "unpublished destination accepted ordinary CONTROL");
    check(ores_native_actor_scheduler_stop(typed, hidden_child),
          "hidden actor cleanup stop failed");
    const OresNativeActorTerminalFailure hidden_failure{
        ORES_NATIVE_CONTROL_PANIC, hidden_child, 101, 20261011
    };
    check(!ores_native_actor_scheduler_publish_failure(
              typed, control_parent, hidden_failure),
          "unpublished candidate forged a typed PANIC envelope");
    check(ores_native_actor_scheduler_seal_normal_exit(typed, hidden_child),
          "rejected hidden PANIC poisoned finalization classification");
    const auto published_child = ores_native_actor_scheduler_spawn(typed, 2);
    check(!published_child.scheduler_domain,
          "hidden scheduler slot unexpectedly permitted a second spawn");
    shutdown(typed);
    std::puts("NATIVE_RESTART_ADMISSION_LEASE_PASS guarded=3 unpublished=1 replay=1 owner=1 reentry=1 control=1");
}
