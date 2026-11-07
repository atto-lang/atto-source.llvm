// Lifecycle commit gate and external-STOP reconciliation: host-only checks.
// No test here is a compiler-backed .atto guest receipt.
#include "oreslang/runtime/RequestLocalStopGroupV1.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

using namespace oreslang::runtime;

namespace {
unsigned passes = 0;
void check(const char* name, bool ok) {
    if (!ok) { std::fprintf(stderr, "ACTOR_LIFECYCLE_FAIL %s\n", name); std::abort(); }
    std::printf("ACTOR_LIFECYCLE_PASS %s\n", name);
    ++passes;
}
void receive(void*, std::size_t, std::uint64_t) {}
int increment(void* ptr) {
    ++*static_cast<int*>(ptr);
    return 1;
}
struct World {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef a{}, b{};
    World() {
        scheduler = ores_native_actor_scheduler_open(2, 8, 2, receive, nullptr);
        assert(scheduler);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        b = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~World() {
        if (ores_native_actor_scheduler_poll_ready(scheduler, a))
            (void)ores_native_actor_scheduler_stop_sealed(scheduler, a);
        if (ores_native_actor_scheduler_poll_ready(scheduler, b))
            (void)ores_native_actor_scheduler_stop_sealed(scheduler, b);
        assert(ores_native_actor_scheduler_wait_done(scheduler, a));
        assert(ores_native_actor_scheduler_wait_done(scheduler, b));
        ores_native_actor_scheduler_retire(scheduler);
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
    }
};
RequestContextLocalV1::Ptr open_request(
        OresNativeActorRef actor, const TaskCancellation::Ptr& authority,
        std::uint64_t id = 500) {
    return RequestContextLocalV1::open({11, 22, id, 1, actor}, authority);
}
struct Pause {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    void park() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&]{ return release; });
    }
    bool await() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(8), [&]{ return entered; });
    }
    void resume() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    }
};
struct PausedMove {
    std::shared_ptr<Pause> pause;
    bool armed;
    explicit PausedMove(std::shared_ptr<Pause> p)
        : pause(std::move(p)), armed(true) {}
    PausedMove(const PausedMove& other)
        : pause(other.pause), armed(false) {}
    PausedMove(PausedMove&& other) noexcept
        : pause(std::move(other.pause)), armed(other.armed) {
        other.armed = false;
        if (armed) { armed = false; pause->park(); }
    }
};

void ordinary_stop() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority);
    RequestLocalKeyV1<int> key;
    int commits = 0;
    check("01_live_commit_gate_admits",
          ores_native_actor_scheduler_commit_if_running_v1(
              w.scheduler, w.a, increment, &commits) && commits == 1);
    check("02_live_store_commit", request->set(key, 7, w.scheduler));
    check("03_live_mail_binding", request->bind_original_mail_once(91, w.scheduler));
    check("04_direct_stop_succeeds", ores_native_actor_scheduler_stop(w.scheduler, w.a));
    check("05_stopped_commit_denied",
          !ores_native_actor_scheduler_commit_if_running_v1(
              w.scheduler, w.a, increment, &commits) && commits == 1);
    check("06_stopped_store_replacement_denied",
          !request->set(key, 8, w.scheduler) &&
          request->get_copy(key) == 7 && request->used_bytes() == sizeof(int));
    check("07_stopped_erase_denied", !request->erase(key, w.scheduler));
    check("08_stopped_mail_rebinding_denied",
          !request->bind_original_mail_once(91, w.scheduler));
    check("09_second_stop_fails", !ores_native_actor_scheduler_stop(w.scheduler, w.a));
    check("10_other_actor_remains_running",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
    authority->retire();
}

void sealed_stop() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority);
    RequestLocalKeyV1<int> key;
    check("11_sealed_initial_commit", request->set(key, 31, w.scheduler));
    check("12_sealed_stop_commits", ores_native_actor_scheduler_stop_sealed(w.scheduler, w.a));
    check("13_sealed_stop_denies_new_value", !request->set(key, 32, w.scheduler));
    check("14_sealed_stop_denies_erase", !request->erase(key, w.scheduler));
    check("15_sealed_stop_does_not_end_sibling",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.b));
    authority->retire();
}

void whole_runtime_retirement() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority);
    RequestLocalKeyV1<int> key;
    check("16_retire_live_before", request->set(key, 12, w.scheduler));
    ores_native_actor_scheduler_retire(w.scheduler);
    check("17_retire_both_lanes", !ores_native_actor_scheduler_poll_ready(w.scheduler,w.a)
                              && !ores_native_actor_scheduler_poll_ready(w.scheduler,w.b));
    check("18_retire_denies_new_value", !request->set(key, 13, w.scheduler));
    check("19_retire_denies_deletion", !request->erase(key, w.scheduler));
    int calls = 0;
    check("20_retire_denies_bounded_callback",
          !ores_native_actor_scheduler_commit_if_running_v1(
              w.scheduler, w.a, increment, &calls) && calls == 0);
    authority->retire();
}

void forged_generations_and_domains() {
    World w;
    auto authority = TaskCancellation::root();
    RequestLocalKeyV1<int> key;
    OresNativeActorRef new_generation = w.a;
    ++new_generation.generation;
    auto forged = open_request(new_generation, authority, 601);
    check("21_forged_generation_store_denied", !forged->set(key, 1, w.scheduler));
    check("22_forged_generation_binding_denied",
          !forged->bind_original_mail_once(1, w.scheduler));
    OresNativeActorRef bad_slot = w.a;
    bad_slot.actor_slot = 99;
    auto invalid_slot = open_request(bad_slot, authority, 602);
    check("23_wrong_slot_store_denied", !invalid_slot->set(key, 2, w.scheduler));
    OresNativeActorRef wrong_domain = w.a;
    ++wrong_domain.scheduler_domain;
    auto invalid_domain = open_request(wrong_domain, authority, 603);
    check("24_wrong_domain_store_denied", !invalid_domain->set(key, 3, w.scheduler));
    check("25_sibling_actor_valid_live",
          open_request(w.b, authority, 604)->set(key, 4, w.scheduler));
    authority->retire();
}

void external_stop_reconciliation() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority, 701);
    RequestLocalStopGroupV1 group(w.scheduler, w.a);
    check("26_external_scope_registered", group.register_request(request));
    check("27_external_STOP_commits", ores_native_actor_scheduler_stop(w.scheduler,w.a));
    // Raw host store remains open at this point: NOT a guest ABI.
    check("28_raw_host_scope_pending_cleanup", request->active());
    check("29_explicit_external_cleanup",
          group.finish_after_external_stop() && !request->active());
    check("30_external_cleanup_marks_DONE", group.completed());
    check("31_repeated_cleanup_denied", !group.finish_after_external_stop());
    authority->retire();
}
void external_runtime_retirement_cleanup() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority, 702);
    RequestLocalStopGroupV1 group(w.scheduler,w.a);
    check("32_retire_cleanup_registration", group.register_request(request));
    ores_native_actor_scheduler_retire(w.scheduler);
    check("33_retire_cleanup_fences_scope",
          group.finish_after_external_stop() && !request->active());
    check("34_retired_guest_commit_still_denied",
          !request->set(RequestLocalKeyV1<int>{},1,w.scheduler));
    authority->retire();
}
void stop_wins_during_precommit() {
    World w;
    auto authority = TaskCancellation::root();
    auto request = open_request(w.a, authority, 703);
    RequestLocalKeyV1<PausedMove> key;
    auto pause = std::make_shared<Pause>();
    std::atomic<bool> committed{true};
    std::thread writer([&] {
        committed.store(request->set(key, PausedMove(pause), w.scheduler));
    });
    const bool reached = pause->await();
    check("35_precommit_value_move_reached", reached);
    check("36_direct_stop_wins", ores_native_actor_scheduler_stop(w.scheduler,w.a));
    pause->resume();
    writer.join();
    check("37_late_precommit_denied", !committed.load());
    check("38_failed_commit_does_not_charge_quota",
          request->used_bytes() == 0 && !request->get_copy(key));
    authority->retire();
}
} // namespace

int main() {
    ordinary_stop();
    sealed_stop();
    whole_runtime_retirement();
    forged_generations_and_domains();
    external_stop_reconciliation();
    external_runtime_retirement_cleanup();
    stop_wins_during_precommit();
    if (passes != 38) return 2;
    std::puts("ACTOR_LIFECYCLE_GUARD_38_HOST_CASES_PASS");
}
