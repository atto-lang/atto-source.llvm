// Source-local host-only lifecycle fence regression. NOT compiled .atto parity.
// The observer publishes only atomics while the scheduler owns its lifecycle
// mutex; closing RequestLocal values, waiting and guest callbacks are forbidden.
#include "oreslang/runtime/NativeActorScheduler.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
int passed = 0;
void check(const char* name, bool condition) {
    if (!condition) {
        std::fprintf(stderr, "ACTOR_TERMINATION_OBSERVER_FAILED %s\n", name);
        std::abort();
    }
    std::printf("ACTOR_TERMINATION_OBSERVER_PASS %s\n", name);
    ++passed;
}
struct Monitor {
    std::atomic<unsigned> calls[3]{};
    std::atomic<unsigned> stopped[3]{};
    std::atomic<unsigned> retired[3]{};
    std::atomic<std::uint64_t> generation[3]{};
    std::atomic<std::uint64_t> domain[3]{};
    std::atomic<unsigned> invalid{0};
    std::atomic<unsigned> called_on_carrier{0};
};
void observer(void* opaque, OresNativeActorRef actor,
              OresNativeActorTerminationKind kind) {
    // No dynamic allocation, locks, awaits, scheduler reentry or destructor.
    auto& m = *static_cast<Monitor*>(opaque);
    if (actor.actor_slot >= 3 || !actor.scheduler_domain || !actor.generation) {
        m.invalid.fetch_add(1);
        return;
    }
    auto i = static_cast<std::size_t>(actor.actor_slot);
    m.domain[i].store(actor.scheduler_domain);
    m.generation[i].store(actor.generation);
    m.calls[i].fetch_add(1);
    if (kind == ORES_NATIVE_TERMINATION_STOPPED)
        m.stopped[i].fetch_add(1);
    else if (kind == ORES_NATIVE_TERMINATION_RUNTIME_RETIRED)
        m.retired[i].fetch_add(1);
    else m.invalid.fetch_add(1);
    if (ores_native_actor_scheduler_is_any_current_carrier_v1())
        m.called_on_carrier.fetch_add(1);
}
void recv(void*, std::size_t, std::uint64_t) {}
OresNativeActorScheduler* open(std::size_t count) {
    auto* s = ores_native_actor_scheduler_open(count, 8, 2, recv, nullptr);
    assert(s);
    return s;
}
bool destroy_after_retirement(OresNativeActorScheduler* s) {
    ores_native_actor_scheduler_retire(s);
    return ores_native_actor_scheduler_drain(s) &&
        ores_native_actor_scheduler_try_destroy(s);
}
void direct_stop_and_retirement() {
    Monitor m;
    auto* s = open(3);
    check("01_null_scheduler_rejected",
          !ores_native_actor_scheduler_configure_termination_observer_v1(
              nullptr, observer, &m));
    check("02_null_observer_rejected",
          !ores_native_actor_scheduler_configure_termination_observer_v1(
              s, nullptr, &m));
    check("03_null_context_rejected",
          !ores_native_actor_scheduler_configure_termination_observer_v1(
              s, observer, nullptr));
    check("04_bootstrap_observer_installed",
          ores_native_actor_scheduler_configure_termination_observer_v1(
              s, observer, &m));
    check("05_observer_cannot_be_replaced",
          !ores_native_actor_scheduler_configure_termination_observer_v1(
              s, observer, &m));
    OresNativeActorRef invalid{0, 0, 1};
    check("06_invalid_stop_denied",
          !ores_native_actor_scheduler_stop(s, invalid));
    check("07_invalid_stop_not_notified", m.calls[0].load() == 0);
    auto a = ores_native_actor_scheduler_spawn(s, 0);
    auto b = ores_native_actor_scheduler_spawn(s, 1);
    auto c = ores_native_actor_scheduler_spawn(s, 2);
    check("08_all_spawns_valid", a.scheduler_domain && b.scheduler_domain &&
          c.scheduler_domain && a.generation && b.generation && c.generation);
    check("09_direct_stop_succeeds",
          ores_native_actor_scheduler_stop(s, a));
    check("10_direct_stop_exactly_once", m.calls[0].load() == 1);
    check("11_direct_stop_kind", m.stopped[0].load() == 1);
    check("12_direct_stop_identity",
          m.domain[0].load() == a.scheduler_domain &&
          m.generation[0].load() == a.generation);
    check("13_sibling_not_revoked",
          m.calls[1].load() == 0 && m.calls[2].load() == 0);
    check("14_double_stop_denied", !ores_native_actor_scheduler_stop(s, a));
    check("15_double_stop_not_renotified", m.calls[0].load() == 1);
    auto stale = a;
    ++stale.generation;
    check("16_generation_aba_stop_denied",
          !ores_native_actor_scheduler_stop(s, stale));
    check("17_stale_generation_not_notified", m.calls[0].load() == 1);
    check("18_sealed_stop_succeeds",
          ores_native_actor_scheduler_stop_sealed(s, b));
    check("19_sealed_stop_exactly_once", m.calls[1].load() == 1);
    check("20_sealed_stop_kind", m.stopped[1].load() == 1);
    check("21_sibling_still_live", ores_native_actor_scheduler_poll_ready(s, c));
    check("22_live_sibling_accepts_message",
          ores_native_actor_scheduler_try_send(s, c, 99) ==
          ORES_NATIVE_SEND_ACCEPTED);
    check("23_no_pre_retire_sibling_notification", m.calls[2].load() == 0);
    ores_native_actor_scheduler_retire(s);
    check("24_retire_notifies_last_live_actor", m.calls[2].load() == 1);
    check("25_retire_kind_distinct", m.retired[2].load() == 1);
    check("26_retire_exact_identity",
          m.domain[2].load() == c.scheduler_domain &&
          m.generation[2].load() == c.generation);
    check("27_retire_does_not_repeat_prior_STOP",
          m.calls[0].load() == 1 && m.calls[1].load() == 1);
    check("28_no_observer_carrier_blocking_path",
          m.called_on_carrier.load() == 0);
    check("29_no_invalid_observer_ref", m.invalid.load() == 0);
    check("30_post_retire_STOP_denied",
          !ores_native_actor_scheduler_stop(s, c));
    OresNativeActorTerminalState terminal{};
    check("31_stop_terminal_receipt",
          ores_native_actor_scheduler_wait_done(s, a) &&
          ores_native_actor_scheduler_terminal_state(s, a, &terminal) &&
          terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    check("32_sealed_terminal_receipt",
          ores_native_actor_scheduler_wait_done(s, b) &&
          ores_native_actor_scheduler_terminal_state(s, b, &terminal) &&
          terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    check("33_retire_terminal_receipt",
          ores_native_actor_scheduler_wait_done(s, c) &&
          ores_native_actor_scheduler_terminal_state(s, c, &terminal) &&
          terminal.kind == ORES_NATIVE_TERMINATION_RUNTIME_RETIRED);
    ores_native_actor_scheduler_retire(s);
    check("34_repeated_retire_idempotent",
          m.calls[0].load() == 1 && m.calls[1].load() == 1 &&
          m.calls[2].load() == 1);
    check("35_drained_before_destroy", ores_native_actor_scheduler_drain(s));
    check("36_destroy_after_retirement", ores_native_actor_scheduler_try_destroy(s));
}
void bootstrap_rejection() {
    Monitor m;
    auto* s = open(1);
    auto a = ores_native_actor_scheduler_spawn(s, 0);
    check("37_spawn_without_observer_allowed", a.scheduler_domain != 0);
    check("38_late_observer_install_denied",
          !ores_native_actor_scheduler_configure_termination_observer_v1(
              s, observer, &m));
    check("39_unobserved_legacy_stop_not_reported",
          ores_native_actor_scheduler_stop(s, a) && m.calls[0].load() == 0);
    check("40_legacy_scheduler_cleanup", destroy_after_retirement(s));
}

// Six competing termination patterns, five repetitions each. An admitted
// STOP wins once, or runtime retirement wins once; never both callbacks.
// This deliberately tests both potential winners without assuming timing.
void termination_races() {
    constexpr int rounds = 5;
    for (int mode = 0; mode != 6; ++mode) {
        for (int iteration = 0; iteration != rounds; ++iteration) {
            Monitor m;
            auto* s = open(1);
            assert(ores_native_actor_scheduler_configure_termination_observer_v1(
                s, observer, &m));
            auto actor = ores_native_actor_scheduler_spawn(s, 0);
            assert(actor.scheduler_domain);
            std::atomic<bool> start{false};
            std::atomic<int> left{-1}, right{-1};
            auto attempt = [&](int side) {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if (mode == 5 || (mode >= 3 && side == 1)) {
                    ores_native_actor_scheduler_retire(s);
                    (side ? right : left).store(2);
                } else {
                    const bool sealed = (mode == 1 && side == 1) ||
                        mode == 2 || mode == 4;
                    int result = sealed
                        ? ores_native_actor_scheduler_stop_sealed(s, actor)
                        : ores_native_actor_scheduler_stop(s, actor);
                    (side ? right : left).store(result);
                }
            };
            std::thread one(attempt, 0), two(attempt, 1);
            start.store(true, std::memory_order_release);
            one.join();
            two.join();
            const auto a = left.load(), b = right.load();
            const bool retire_ran = a == 2 || b == 2;
            const bool stop_won = a == 1 || b == 1;
            const bool winners_valid = mode < 3
                ? (a + b == 1)
                : (mode == 5 ? a == 2 && b == 2
                              : ((a == 0 || a == 1) && b == 2));
            const bool kind_consistent =
                m.calls[0].load() == 1 &&
                m.stopped[0].load() == (stop_won ? 1u : 0u) &&
                m.retired[0].load() == (stop_won ? 0u : 1u);
            const bool terminal = ores_native_actor_scheduler_wait_done(s, actor);
            OresNativeActorTerminalState output{};
            const bool receipt = ores_native_actor_scheduler_terminal_state(
                s, actor, &output) &&
                output.kind == (stop_won
                    ? ORES_NATIVE_TERMINATION_STOPPED
                    : ORES_NATIVE_TERMINATION_RUNTIME_RETIRED);
            char name[96];
            std::snprintf(name, sizeof(name), "race_%d_%d_one_winner_and_receipt",
                          mode, iteration);
            check(name, winners_valid && (mode < 3 || retire_ran) &&
                  kind_consistent && terminal && receipt &&
                  m.invalid.load() == 0);
            check("race_no_repeated_notification_after_retire",
                  (ores_native_actor_scheduler_retire(s), m.calls[0].load() == 1));
            check("race_cleanup", ores_native_actor_scheduler_drain(s) &&
                  ores_native_actor_scheduler_try_destroy(s));
        }
    }
}
} // namespace

int main() {
    direct_stop_and_retirement();
    bootstrap_rejection();
    termination_races();
    check("41_at_least_20_real_termination_race_trials", passed >= 130);
    std::printf("ACTOR_TERMINATION_OBSERVER_HOST_PASS total=%d race_trials=30\n", passed);
}
