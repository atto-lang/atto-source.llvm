// Trusted host-only RequestLocal ingress and supervisor STOP integration.
// Not a compiler-backed .atto execution test, not physical actor restart,
// and not runtime issuer authentication of tenant/isolate provenance.
#include "oreslang/runtime/RequestLocalActorTerminationRegistryV1.hpp"

#include <array>
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
using Registry = RequestLocalActorTerminationRegistryV1;
int passed = 0;
void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "INGRESS_SUPERVISOR_CASE_FAILED %s\n", name);
        std::abort();
    }
    std::printf("INGRESS_SUPERVISOR_CASE_PASS %s\n", name);
    ++passed;
}
void receive(void*, std::size_t, std::uint64_t) {}

OresNativeActorScheduler* scheduler_with_two_slots() {
    auto* s = ores_native_actor_scheduler_open(2, 8, 2, receive, nullptr);
    assert(s);
    return s;
}
RequestIdentityV1 identity(OresNativeActorRef actor, std::uint64_t id) {
    return RequestIdentityV1{11, 22, id, 1, actor};
}
struct World {
    OresNativeActorScheduler* scheduler = scheduler_with_two_slots();
    Registry::Ptr registry;
    OresNativeActorRef a{}, b{};
    World() {
        registry = Registry::create(scheduler, 2);
        assert(registry);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        b = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~World() {
        assert(registry->shutdown_after_retire());
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
        registry.reset();
    }
};
void bootstrap_capacity() {
    auto* s = scheduler_with_two_slots();
    check("01_null_scheduler_capacity",
          ores_native_actor_scheduler_actor_capacity_v1(nullptr) == 0);
    check("02_exact_scheduler_actor_capacity",
          ores_native_actor_scheduler_actor_capacity_v1(s) == 2);
    check("03_undersized_registry_rejected", !Registry::create(s, 1));
    check("04_oversized_registry_rejected", !Registry::create(s, 3));
    check("05_zero_registry_rejected", !Registry::create(s, 0));
    auto registry = Registry::create(s, 2);
    check("06_exact_capacity_installed", bool(registry));
    check("07_duplicate_observer_install_denied",
          !Registry::create(s, 2));
    auto a = ores_native_actor_scheduler_spawn(s, 0);
    check("08_first_actor_spawned", a.scheduler_domain != 0);
    check("09_observer_installed_before_first_actor",
          !Registry::create(s, 2));
    check("10_registry_shutdown_retires_scheduler",
          registry->shutdown_after_retire());
    check("11_post_shutdown_repeat_denied",
          !registry->shutdown_after_retire());
    check("12_retired_actor_terminal",
          ores_native_actor_scheduler_poll_done(s, a));
    check("13_retired_scheduler_drains",
          ores_native_actor_scheduler_drain(s));
    check("14_retired_scheduler_destroys",
          ores_native_actor_scheduler_try_destroy(s));
    registry.reset();
}
void ingress_authority() {
    World w;
    auto c = TaskCancellation::root();
    RequestLocalKeyV1<int> key;
    auto invalid = identity(w.a, 100);
    invalid.tenant = 0;
    check("15_no_tenant_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 101);
    invalid.isolate = 0;
    check("16_no_isolate_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 102);
    invalid.request = 0;
    check("17_no_request_id_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 103);
    invalid.generation = 0;
    check("18_no_request_generation_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 104);
    ++invalid.actor.generation;
    check("19_invalid_actor_generation_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 105);
    ++invalid.actor.scheduler_domain;
    check("20_invalid_scheduler_domain_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    invalid = identity(w.a, 106);
    invalid.actor.actor_slot = 2;
    check("21_out_of_range_actor_rejected",
          !w.registry->admit_from_trusted_ingress(invalid, c));
    check("22_null_cancellation_rejected",
          !w.registry->admit_from_trusted_ingress(
              identity(w.a, 107), {}));
    auto req = w.registry->admit_from_trusted_ingress(identity(w.a, 108), c);
    check("23_valid_request_registered", bool(req) && req->active());
    check("24_unregistered_raw_set_bypass_denied",
          !req->set(key, 11));
    check("25_authenticated_live_set_allowed",
          req->set(key, 12, w.scheduler));
    check("26_live_read_copy", req->get_copy(key) == 12);
    check("27_null_scheduler_raw_erase_denied",
          !req->erase(key));
    check("28_lifecycle_stop_committed",
          w.registry->signal_supervisor_termination(
              w.a, Registry::SupervisorCause::normal_stop));
    check("29_immediate_request_revocation", !req->active());
    check("30_request_must_not_escape_after_stop",
          !req->get_copy(key) && !req->set(key, 13, w.scheduler));
    check("31_external_worker_reaped_request",
          w.registry->wait_reaped(w.a));
    check("32_reclaimed_payload_accounting", req->used_bytes() == 0);
    check("33_late_ingress_denied",
          !w.registry->admit_from_trusted_ingress(identity(w.a, 109), c));
    check("34_duplicate_stop_denied",
          !w.registry->signal_supervisor_termination(
              w.a, Registry::SupervisorCause::normal_stop));
    check("35_sibling_actor_unaffected",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
    c->retire();
}
void cancel_before_ingress() {
    World w;
    auto cancelled = TaskCancellation::root();
    check("36_cancel_preexisting_authority",
          cancelled->cancel("prior cancellation"));
    auto req = w.registry->admit_from_trusted_ingress(
        identity(w.a, 121), cancelled);
    check("37_cancelled_scope_not_exposed", !req);
    check("38_cancellation_not_actor_STOP",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.a));
    cancelled->retire();
}
void supervisor_stop_causes() {
    const std::array<Registry::SupervisorCause, 6> causes{
        Registry::SupervisorCause::normal_stop,
        Registry::SupervisorCause::cancellation,
        Registry::SupervisorCause::panic,
        Registry::SupervisorCause::supervisor_shutdown,
        Registry::SupervisorCause::restart,
        Registry::SupervisorCause::hot_reload
    };
    for (std::size_t i = 0; i < causes.size(); ++i) {
        World w;
        auto c = TaskCancellation::root();
        auto req = w.registry->admit_from_trusted_ingress(
            identity(w.a, 200 + i), c);
        char label[100];
        std::snprintf(label, sizeof(label),
                      "cause_%zu_scope_admitted_before_stop", i);
        check(label, bool(req));
        std::snprintf(label, sizeof(label),
                      "cause_%zu_signal_accepted", i);
        check(label, w.registry->signal_supervisor_termination(w.a, causes[i]));
        std::snprintf(label, sizeof(label),
                      "cause_%zu_immediate_revoke", i);
        check(label, !req->active());
        std::snprintf(label, sizeof(label),
                      "cause_%zu_reap_and_done", i);
        check(label, w.registry->wait_reaped(w.a) &&
              ores_native_actor_scheduler_poll_done(w.scheduler, w.a));
        std::snprintf(label, sizeof(label),
                      "cause_%zu_duplicate_denied", i);
        check(label, !w.registry->signal_supervisor_termination(w.a, causes[i]));
        std::snprintf(label, sizeof(label),
                      "cause_%zu_sibling_survives", i);
        check(label, ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
        auto stale = w.a;
        ++stale.generation;
        std::snprintf(label, sizeof(label),
                      "cause_%zu_generation_ABA_denied", i);
        check(label, !w.registry->signal_supervisor_termination(stale, causes[i]));
        std::snprintf(label, sizeof(label),
                      "cause_%zu_slot_revival_not_claimed", i);
        check(label,
              !ores_native_actor_scheduler_spawn(w.scheduler, 0).scheduler_domain);
        c->retire();
    }
}
void admission_vs_supervisor_stop_matrix() {
    const std::array<Registry::SupervisorCause, 6> causes{
        Registry::SupervisorCause::normal_stop,
        Registry::SupervisorCause::cancellation,
        Registry::SupervisorCause::panic,
        Registry::SupervisorCause::supervisor_shutdown,
        Registry::SupervisorCause::restart,
        Registry::SupervisorCause::hot_reload
    };
    // 60 separately labeled two-thread adversarial schedules. Either
    // ingress or STOP wins, but no admitted request survives revocation.
    for (std::size_t kind = 0; kind < causes.size(); ++kind) {
        for (int trial = 0; trial < 10; ++trial) {
            World w;
            auto c = TaskCancellation::root();
            RequestContextLocalV1::Ptr req;
            std::atomic<bool> start{false}, stopped{false};
            std::thread ingress([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                req = w.registry->admit_from_trusted_ingress(
                    identity(w.a, 300 + kind * 10 + trial), c);
            });
            std::thread stopping([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                stopped.store(w.registry->signal_supervisor_termination(
                    w.a, causes[kind]));
            });
            start.store(true, std::memory_order_release);
            ingress.join();
            stopping.join();
            char label[128];
            std::snprintf(label, sizeof(label),
                          "race_%zu_%02d_no_request_after_STOP", kind, trial);
            check(label, stopped.load() &&
                  (!req || !req->active()) &&
                  !w.registry->admit_from_trusted_ingress(
                      identity(w.a, 900 + kind * 10 + trial), c));
            std::snprintf(label, sizeof(label),
                          "race_%zu_%02d_exact_reap_and_sibling", kind, trial);
            check(label, w.registry->wait_reaped(w.a) &&
                  w.registry->reaped_actors() == 1 &&
                  ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
            c->retire();
        }
    }
}
} // namespace

int main() {
    bootstrap_capacity();
    ingress_authority();
    cancel_before_ingress();
    supervisor_stop_causes();
    admission_vs_supervisor_stop_matrix();
    // 38 fixed + 48 stop-cause assertions + 120 race assertions + final.
    check("final_expected_207_host_assertions", passed == 206);
    std::printf("REQUEST_INGRESS_SUPERVISOR_HOST_PASS total=%d races=60\n",
                passed);
}
