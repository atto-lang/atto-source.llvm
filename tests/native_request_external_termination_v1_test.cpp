// Native host CONTROL cleanup after externally initiated STOP / retirement.
// No .atto compiler-backed execution and no automatic global RequestLocal sweep.
#include "oreslang/runtime/RequestLocalStopGroupV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace oreslang::runtime;
namespace {
int passed = 0;
void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "EXTERNAL_REQUEST_STOP_FAILED %s\n", name);
        std::abort();
    }
    std::printf("EXTERNAL_REQUEST_STOP_PASS %s\n", name);
    ++passed;
}
void receive(void*, std::size_t, std::uint64_t) {}
struct Fixture {
    OresNativeActorScheduler* s = nullptr;
    OresNativeActorRef a{}, b{};
    Fixture() {
        s = ores_native_actor_scheduler_open(2, 8, 2, receive, nullptr);
        assert(s);
        a = ores_native_actor_scheduler_spawn(s, 0);
        b = ores_native_actor_scheduler_spawn(s, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~Fixture() {
        (void)ores_native_actor_scheduler_stop(s, a);
        (void)ores_native_actor_scheduler_stop(s, b);
        assert(ores_native_actor_scheduler_wait_done(s, a));
        assert(ores_native_actor_scheduler_wait_done(s, b));
        ores_native_actor_scheduler_retire(s);
        assert(ores_native_actor_scheduler_drain(s));
        assert(ores_native_actor_scheduler_try_destroy(s));
    }
};
RequestContextLocalV1::Ptr request(
    OresNativeActorRef actor, std::uint64_t id, TaskCancellation::Ptr cancel) {
    return RequestContextLocalV1::open(
        RequestIdentityV1{31, 41, id, 1, actor}, std::move(cancel));
}
void ordinary_stop_then_sweep() {
    Fixture f;
    auto ca = TaskCancellation::root();
    auto cb = TaskCancellation::root();
    auto ra = request(f.a, 1, ca), rb = request(f.b, 2, cb);
    RequestLocalStopGroupV1 a(f.s, f.a), b(f.s, f.b);
    RequestLocalKeyV1<std::uint64_t> key;
    check("01_register_live_request_a", a.register_request(ra));
    check("02_register_live_request_b", b.register_request(rb));
    check("03_live_actor_cannot_be_reaped",
          !a.reap_after_external_termination_and_wait());
    check("04_failed_pre_stop_reap_preserves_request",
          !a.sealed() && ra->active() && ra->set(key, 11));
    check("05_direct_scheduler_stop", ores_native_actor_scheduler_stop(f.s, f.a));
    check("06_post_stop_registration_denied", !a.register_request(ra));
    check("07_cleanup_after_direct_STOP",
          a.reap_after_external_termination_and_wait());
    check("08_exact_request_revoked",
          !ra->active() && !ra->get_copy(key));
    check("09_group_sealed_and_completed", a.sealed() && a.completed());
    check("10_repeated_sweep_denied",
          !a.reap_after_external_termination_and_wait());
    check("11_no_repeat_stop_after_external_sweep",
          !a.stop_sealed_and_wait());
    check("12_sibling_group_unaffected",
          !b.sealed() && rb->active() &&
          ores_native_actor_scheduler_poll_ready(f.s, f.b));
    check("13_sibling_still_accepts_DATA",
          ores_native_actor_scheduler_try_send(f.s, f.b, 37) ==
          ORES_NATIVE_SEND_ACCEPTED);
    check("14_sibling_separate_STOP", b.stop_sealed_and_wait());
    check("15_sibling_request_revoked", !rb->active());
    ca->retire();
    cb->retire();
}
void cancel_and_sealed_stop() {
    Fixture f;
    auto ca = TaskCancellation::root();
    auto cb = TaskCancellation::root();
    auto ra = request(f.a, 3, ca), rb = request(f.a, 4, cb);
    RequestLocalStopGroupV1 group(f.s, f.a);
    check("16_first_request_registered", group.register_request(ra));
    check("17_second_request_registered", group.register_request(rb));
    check("18_cancel_first_request", ca->cancel("pre-STOP cancellation"));
    check("19_cancel_closes_only_first", !ra->active() && rb->active());
    check("20_external_sealed_STOP",
          ores_native_actor_scheduler_stop_sealed(f.s, f.a));
    check("21_sealed_STOP_reap",
          group.reap_after_external_termination_and_wait());
    check("22_cancelled_and_live_request_both_closed",
          !ra->active() && !rb->active());
    check("23_cancel_idempotence_after_STOP",
          !ca->cancel("duplicate cancellation"));
    check("24_no_new_registration_after_sealed_STOP",
          !group.register_request(ra));
    check("25_terminal_kind_stopped",
          ores_native_actor_scheduler_poll_done(f.s, f.a));
    ca->retire();
    cb->retire();
}
void runtime_retire_and_fence() {
    Fixture f;
    auto ca = TaskCancellation::root(), cb = TaskCancellation::root();
    auto ra = request(f.a, 5, ca), rb = request(f.b, 6, cb);
    RequestLocalStopGroupV1 a(f.s, f.a), b(f.s, f.b);
    check("26_register_before_retirement",
          a.register_request(ra) && b.register_request(rb));
    ores_native_actor_scheduler_retire(f.s);
    check("27_runtime_retire_denies_readmission",
          !a.register_request(ra) && !b.register_request(rb));
    check("28_runtime_retire_reaps_first_actor",
          a.reap_after_external_termination_and_wait());
    check("29_runtime_retire_reaps_second_actor",
          b.reap_after_external_termination_and_wait());
    check("30_both_requests_closed", !ra->active() && !rb->active());
    OresNativeActorTerminalState ta{}, tb{};
    check("31_runtime_retire_terminal_receipt",
          ores_native_actor_scheduler_terminal_state(f.s, f.a, &ta) &&
          ores_native_actor_scheduler_terminal_state(f.s, f.b, &tb) &&
          ta.kind == ORES_NATIVE_TERMINATION_RUNTIME_RETIRED &&
          tb.kind == ORES_NATIVE_TERMINATION_RUNTIME_RETIRED);
    check("32_double_retirement_sweep_denied",
          !a.reap_after_external_termination_and_wait());
    ca->retire();
    cb->retire();
}
void generation_and_slot_aba() {
    Fixture f;
    auto ca = TaskCancellation::root();
    auto ra = request(f.a, 7, ca);
    RequestLocalStopGroupV1 current(f.s, f.a);
    auto stale = f.a;
    ++stale.generation;
    RequestLocalStopGroupV1 wrong_generation(f.s, stale);
    check("33_stale_generation_cannot_register_live_request",
          !wrong_generation.register_request(ra));
    check("34_current_generation_can_register", current.register_request(ra));
    check("35_wrong_generation_STOP_denied",
          !ores_native_actor_scheduler_stop(f.s, stale));
    check("36_wrong_generation_not_reapable",
          !wrong_generation.reap_after_external_termination_and_wait());
    check("37_original_actor_survives_stale_STOP",
          ores_native_actor_scheduler_poll_ready(f.s, f.a) && ra->active());
    check("38_stop_current_incarnation",
          ores_native_actor_scheduler_stop(f.s, f.a));
    check("39_stopped_actor_slot_cannot_respawn",
          !ores_native_actor_scheduler_spawn(f.s, 0).scheduler_domain);
    check("40_stale_request_rejected_after_STOP",
          !current.register_request(ra));
    check("41_current_generation_cleanup",
          current.reap_after_external_termination_and_wait() && !ra->active());
    ca->retire();
}
void stop_vs_cancel_race() {
    // Repeated independent races; either cancellation or STOP may win but
    // the cleanup barrier must still close the exact request once.
    for (int i = 0; i < 20; ++i) {
        Fixture f;
        auto cancel = TaskCancellation::root();
        auto req = request(f.a, 100 + static_cast<std::uint64_t>(i), cancel);
        RequestLocalStopGroupV1 group(f.s, f.a);
        assert(group.register_request(req));
        std::atomic<bool> go{false};
        std::thread stop([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)ores_native_actor_scheduler_stop(f.s, f.a);
        });
        std::thread revoke([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)cancel->cancel("racing stop");
        });
        go.store(true, std::memory_order_release);
        stop.join();
        revoke.join();
        char name[96];
        std::snprintf(name, sizeof(name),
                      "race_%02d_stop_vs_cancellation_reaped", i);
        check(name, group.reap_after_external_termination_and_wait() &&
              group.completed() && !req->active());
        cancel->retire();
    }
}
} // namespace
int main() {
    ordinary_stop_then_sweep();
    cancel_and_sealed_stop();
    runtime_retire_and_fence();
    generation_and_slot_aba();
    stop_vs_cancel_race();
    check("42_at_least_20_adversarial_cancel_STOP_trials", passed == 61);
    std::printf("REQUEST_EXTERNAL_STOP_HOST_PASS total=%d race_trials=20\n", passed);
}
