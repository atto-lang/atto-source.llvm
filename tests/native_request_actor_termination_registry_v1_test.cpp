// Native HOST-only actor-incarnation RequestLocal registry regression.
// No .atto guest/compiler ownership or backend parity is established here.
#include "oreslang/runtime/RequestLocalActorTerminationRegistryV1.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace oreslang::runtime;
namespace {
int passes = 0;
void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "REQUEST_REGISTRY_CASE_FAILED %s\n", name);
        std::abort();
    }
    std::printf("REQUEST_REGISTRY_CASE_PASS %s\n", name);
    ++passes;
}

std::atomic<RequestLocalActorTerminationRegistryV1*> carrier_target{nullptr};
std::atomic<bool> carrier_wait_denied{false};
void receive(void*, std::size_t, std::uint64_t payload) {
    if (payload != 99) return;
    auto* registry = carrier_target.load(std::memory_order_acquire);
    if (registry)
        carrier_wait_denied.store(
            !registry->wait_reaped(OresNativeActorRef{1, 0, 1}),
            std::memory_order_release);
}

struct World {
    OresNativeActorScheduler* scheduler;
    RequestLocalActorTerminationRegistryV1::Ptr registry;
    OresNativeActorRef a{}, b{};
    explicit World(bool spawn_second = true) {
        scheduler = ores_native_actor_scheduler_open(
            2, 8, 2, receive, nullptr);
        assert(scheduler);
        registry = RequestLocalActorTerminationRegistryV1::create(
            scheduler, 2);
        assert(registry);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        assert(a.scheduler_domain);
        if (spawn_second) {
            b = ores_native_actor_scheduler_spawn(scheduler, 1);
            assert(b.scheduler_domain);
        }
    }
    ~World() {
        assert(registry->shutdown_after_retire());
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
        registry.reset();
    }
    RequestContextLocalV1::Ptr request(OresNativeActorRef actor,
                                       std::uint64_t id,
                                       TaskCancellation::Ptr cancellation) {
        return RequestContextLocalV1::open(
            RequestIdentityV1{11, 22, id, 1, actor},
            std::move(cancellation));
    }
};

void bootstrap_and_registration() {
    check("01_null_scheduler_rejected",
          !RequestLocalActorTerminationRegistryV1::create(nullptr, 1));
    World w;
    auto c = TaskCancellation::root();
    auto a = w.request(w.a, 1, c);
    auto b = w.request(w.b, 2, c);
    check("02_request_admitted", w.registry->admit(w.a, a));
    check("03_duplicate_registration_denied",
          !w.registry->admit(w.a, a));
    check("04_foreign_actor_denied", !w.registry->admit(w.a, b));
    check("05_unadmitted_actor_not_reaped",
          !w.registry->wait_reaped(w.b));
    RequestLocalKeyV1<int> key;
    check("06_admitted_write", a->set(key, 7, w.scheduler));
    check("registered_raw_write_denied", !a->set(key, 8));
    check("registered_raw_erase_denied", !a->erase(key));
    check("registered_raw_mail_binding_denied",
          !a->bind_original_mail_once(123));
    check("07_admitted_copy", a->get_copy(key) == 7);
    check("08_direct_STOP", ores_native_actor_scheduler_stop(w.scheduler, w.a));
    check("09_immediate_atomic_revocation", !a->active());
    check("10_read_denied_after_STOP", !a->get_copy(key));
    check("11_write_denied_after_STOP",
          !a->set(key, 8, w.scheduler));
    check("12_erase_denied_after_STOP", !a->erase(key, w.scheduler));
    check("13_async_CONTROL_reclaims_scope",
          w.registry->wait_reaped(w.a));
    check("14_quota_reclaimed", a->used_bytes() == 0);
    check("15_nonregistered_sibling_survives",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
    check("16_duplicate_STOP_denied",
          !ores_native_actor_scheduler_stop(w.scheduler, w.a));
    check("17_exactly_one_actor_reaped",
          w.registry->reaped_actors() == 1);
    auto stale = w.a;
    ++stale.generation;
    check("18_stale_generation_reap_denied",
          !w.registry->wait_reaped(stale));
    check("19_stale_generation_admission_denied",
          !w.registry->admit(stale, w.request(stale, 3, c)));
    check("20_post_STOP_admission_denied",
          !w.registry->admit(w.a, w.request(w.a, 4, c)));
    check("21_stopped_slot_cannot_respawn",
          !ores_native_actor_scheduler_spawn(w.scheduler, 0).scheduler_domain);
    check("22_no_invalid_notification", w.registry->rejected_terminations() == 0);
    c->retire();
}

void capacity_and_retirement() {
    World w;
    auto cancellation = TaskCancellation::root();
    std::array<RequestContextLocalV1::Ptr, 33> requests{};
    bool filled = true;
    for (std::size_t i = 0; i < 32; ++i) {
        requests[i] = w.request(w.a, i + 100, cancellation);
        filled = filled && w.registry->admit(w.a, requests[i]);
    }
    check("23_preallocated_registry_capacity", filled);
    requests[32] = w.request(w.a, 132, cancellation);
    check("24_excess_request_denied",
          !w.registry->admit(w.a, requests[32]));
    check("25_second_registration_remains_open",
          requests[0]->active());
    ores_native_actor_scheduler_retire(w.scheduler);
    check("26_retirement_revokes_first", !requests[0]->active());
    check("27_retirement_revokes_last", !requests[31]->active());
    check("28_retirement_reap", w.registry->wait_reaped(w.a));
    check("29_reaped_scope_used_bytes",
          requests[0]->used_bytes() == 0);
    check("30_retirement_reaps_unregistered_sibling",
          w.registry->wait_reaped(w.b));
    check("31_second_termination_kind_fenced",
          !w.registry->admit(w.a, w.request(w.a, 133, cancellation)));
    check("32_reaped_exactly_two_actor_slots",
          w.registry->reaped_actors() == 2);
    cancellation->retire();
}

void cancellation_and_sealed_stop() {
    World w;
    auto cancellation = TaskCancellation::root();
    auto req = w.request(w.a, 200, cancellation);
    check("33_cancelled_request_initially_admitted",
          w.registry->admit(w.a, req));
    check("34_cancellation_revokes",
          cancellation->cancel("cancel before sealed STOP"));
    check("35_cancellation_closes_request", !req->active());
    check("36_sealed_STOP_after_cancel",
          ores_native_actor_scheduler_stop_sealed(w.scheduler, w.a));
    check("37_reaped_cancelled_scope", w.registry->wait_reaped(w.a));
    check("38_cancelled_scope_zero_quota", req->used_bytes() == 0);
    check("39_sibling_remains_ready",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
    check("40_repeated_cancel_denied",
          !cancellation->cancel("duplicate"));
    cancellation->retire();
}

void carrier_must_not_wait() {
    World w;
    auto cancellation = TaskCancellation::root();
    auto req = w.request(w.a, 300, cancellation);
    check("41_request_admitted_before_carrier",
          w.registry->admit(w.a, req));
    carrier_wait_denied.store(false);
    carrier_target.store(w.registry.get(), std::memory_order_release);
    check("42_carrier_callback_admitted",
          ores_native_actor_scheduler_try_send(w.scheduler, w.b, 99) ==
          ORES_NATIVE_SEND_ACCEPTED);
    check("43_carrier_dispatch_completed",
          ores_native_actor_scheduler_drain(w.scheduler));
    carrier_target.store(nullptr, std::memory_order_release);
    check("44_carrier_CANNOT_block_reaper",
          carrier_wait_denied.load(std::memory_order_acquire));
    check("45_external_CONTROL_can_stop",
          ores_native_actor_scheduler_stop(w.scheduler, w.a));
    check("46_external_CONTROL_can_join", w.registry->wait_reaped(w.a));
    cancellation->retire();
}

void registration_STOP_races() {
    // Each winner permitted; successful admissions are always revoked,
    // unsuccessful applications cannot resurrect a stopped incarnation.
    for (int round = 0; round != 30; ++round) {
        World w(false);
        auto cancellation = TaskCancellation::root();
        auto req = w.request(w.a, 400 + round, cancellation);
        std::atomic<bool> go{false}, admitted{false}, stopped{false};
        std::thread registerer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            admitted.store(w.registry->admit(w.a, req));
        });
        std::thread stopper([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            stopped.store(ores_native_actor_scheduler_stop(w.scheduler, w.a));
        });
        go.store(true, std::memory_order_release);
        registerer.join();
        stopper.join();
        char name[96];
        std::snprintf(name, sizeof(name),
                      "race_%02d_registration_vs_STOP", round);
        check(name, stopped.load() &&
              w.registry->wait_reaped(w.a) &&
              (!admitted.load() || !req->active()) &&
              w.registry->reaped_actors() == 1);
        req->close(); // release unadmitted host-only request if STOP won
        cancellation->retire();
    }
}

void cancellation_STOP_races() {
    for (int round = 0; round != 30; ++round) {
        World w(false);
        auto cancellation = TaskCancellation::root();
        auto req = w.request(w.a, 500 + round, cancellation);
        assert(w.registry->admit(w.a, req));
        std::atomic<bool> go{false}, stopped{false};
        std::thread stopper([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            stopped.store(ores_native_actor_scheduler_stop_sealed(w.scheduler, w.a));
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)cancellation->cancel("racing cancellation");
        });
        go.store(true, std::memory_order_release);
        stopper.join();
        canceller.join();
        char name[96];
        std::snprintf(name, sizeof(name),
                      "race_%02d_cancellation_vs_sealed_STOP", round);
        check(name, stopped.load() &&
              w.registry->wait_reaped(w.a) &&
              !req->active() &&
              w.registry->reaped_actors() == 1);
        cancellation->retire();
    }
}
} // namespace

int main() {
    bootstrap_and_registration();
    capacity_and_retirement();
    cancellation_and_sealed_stop();
    carrier_must_not_wait();
    registration_STOP_races();
    cancellation_STOP_races();
    check("47_all_60_coordinated_races_executed", passes == 109);
    std::printf("REQUEST_TERMINATION_REGISTRY_HOST_PASS total=%d race_trials=60\n",
                passes);
}
