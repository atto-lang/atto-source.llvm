// 30 named host-level STOP/cancellation/ownership regression cases.
// These are NOT compiled .atto guest tests. No source guest can construct
// the opaque C++ values or directly use RequestLocalStopGroupV1.
#include "oreslang/runtime/RequestLocalStopGroupV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace oreslang::runtime;
namespace {
int passed = 0;
void check(const char* name, bool good) {
    if (!good) {
        std::fprintf(stderr, "REQUEST_STOP_CASE_FAILED %s\n", name);
        std::abort();
    }
    std::printf("REQUEST_STOP_CASE_PASS %s\n", name);
    ++passed;
}

struct Pause {
    std::mutex mu;
    std::condition_variable cv;
    bool reached = false;
    bool released = false;
    void park() {
        std::unique_lock<std::mutex> lock(mu);
        reached = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool await() {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(8),
                           [&] { return reached; });
    }
    void resume() {
        std::lock_guard<std::mutex> lock(mu);
        released = true;
        cv.notify_all();
    }
};

// User-defined host moves and copies must happen OUTSIDE the request store's
// cancellation-graph mutex; they can stop the world and permit cancellation.
struct PauseOnMove {
    std::shared_ptr<Pause> pause;
    bool armed;
    explicit PauseOnMove(std::shared_ptr<Pause> p)
        : pause(std::move(p)), armed(true) {}
    PauseOnMove(const PauseOnMove& other)
        : pause(other.pause), armed(false) {}
    PauseOnMove(PauseOnMove&& other) noexcept
        : pause(std::move(other.pause)), armed(other.armed) {
        other.armed = false;
        if (armed) { armed = false; pause->park(); }
    }
};
struct PauseOnCopy {
    std::shared_ptr<Pause> pause;
    bool armed;
    explicit PauseOnCopy(std::shared_ptr<Pause> p)
        : pause(std::move(p)), armed(true) {}
    PauseOnCopy(const PauseOnCopy& other)
        : pause(other.pause), armed(other.armed) {
        if (armed) { armed = false; pause->park(); }
    }
    PauseOnCopy(PauseOnCopy&& other) noexcept
        : pause(std::move(other.pause)), armed(other.armed) {
        other.armed = false;
    }
};

std::atomic<unsigned> received{0};
void receive(void*, std::size_t, std::uint64_t) {
    ++received;
}

struct Actors {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef a{}, b{};
    Actors() {
        scheduler = ores_native_actor_scheduler_open(2, 8, 2, receive, nullptr);
        assert(scheduler);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        b = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~Actors() {
        (void)ores_native_actor_scheduler_stop(scheduler, a);
        (void)ores_native_actor_scheduler_stop(scheduler, b);
        assert(ores_native_actor_scheduler_wait_done(scheduler, a));
        assert(ores_native_actor_scheduler_wait_done(scheduler, b));
        ores_native_actor_scheduler_retire(scheduler);
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
    }
};

RequestContextLocalV1::Ptr new_request(
        OresNativeActorRef actor, std::uint64_t id,
        const TaskCancellation::Ptr& cancellation) {
    return RequestContextLocalV1::open(
        RequestIdentityV1{11, 22, id, 1, actor}, cancellation);
}

void stop_and_sibling_survival() {
    Actors actors;
    auto c1 = TaskCancellation::root();
    auto c2 = TaskCancellation::root();
    auto cb = TaskCancellation::root();
    auto a1 = new_request(actors.a, 1, c1);
    auto a2 = new_request(actors.a, 2, c2);
    auto b = new_request(actors.b, 3, cb);
    RequestLocalStopGroupV1 stop(actors.scheduler, actors.a);
    check("01_register_request_1", stop.register_request(a1));
    check("02_register_request_2", stop.register_request(a2));
    check("03_duplicate_registration_denied", !stop.register_request(a1));
    check("04_foreign_actor_registration_denied", !stop.register_request(b));
    check("05_open_before_stop", a1->active() && a2->active());
    check("06_control_plane_stop_and_quiescence", stop.stop_sealed_and_wait());
    check("07_first_request_closed", !a1->active());
    check("08_second_request_closed", !a2->active());
    check("09_stop_sealed_no_readmission",
          stop.completed() && stop.sealed() && !stop.register_request(a1));
    const unsigned prior = received.load();
    check("10_sibling_actor_remains_live",
          ores_native_actor_scheduler_try_send(
              actors.scheduler, actors.b, 42) == ORES_NATIVE_SEND_ACCEPTED &&
          ores_native_actor_scheduler_drain(actors.scheduler) &&
          received.load() == prior + 1);
    cb->retire();
    c2->retire();
    c1->retire();
}

void stop_wins_before_writer_commit() {
    Actors actors;
    auto c = TaskCancellation::root();
    auto request = new_request(actors.a, 4, c);
    RequestLocalStopGroupV1 stop(actors.scheduler, actors.a);
    RequestLocalKeyV1<PauseOnMove> key;
    auto pause = std::make_shared<Pause>();
    check("11_stop_race_scope_registered", stop.register_request(request));
    std::atomic<bool> result{true};
    std::thread writer([&] {
        result.store(request->set(key, PauseOnMove(pause)));
    });
    const bool reached = pause->await();
    check("12_writer_paused_before_commit", reached);
    check("13_stop_seals_before_writer_released",
          stop.stop_sealed_and_wait());
    pause->resume();
    writer.join();
    check("14_stale_writer_commit_denied", !result.load());
    check("15_no_post_stop_quota_or_value",
          request->used_bytes() == 0 && !request->get_copy(key));
    c->retire();
}

void cancel_wins_before_writer_commit() {
    auto c = TaskCancellation::root();
    auto request = new_request({71, 8, 1}, 5, c);
    RequestLocalKeyV1<PauseOnMove> key;
    auto pause = std::make_shared<Pause>();
    check("16_cancellation_race_initially_active", request->active());
    std::atomic<bool> result{true};
    std::thread writer([&] {
        result.store(request->set(key, PauseOnMove(pause)));
    });
    const bool reached = pause->await();
    check("17_writer_paused_during_value_move", reached);
    check("18_cancel_wins_during_precommit", c->cancel("cancel-while-moving"));
    pause->resume();
    writer.join();
    check("19_post_cancel_write_denied", !result.load());
    check("20_post_cancel_quota_zero",
          request->used_bytes() == 0 && !request->active());
    c->retire();
}

void close_wins_during_read_copy() {
    auto c = TaskCancellation::root();
    auto request = new_request({71, 8, 1}, 6, c);
    RequestLocalKeyV1<PauseOnCopy> key;
    auto pause = std::make_shared<Pause>();
    check("21_read_copy_source_installed",
          request->set(key, PauseOnCopy(pause)));
    std::atomic<bool> got{true};
    std::thread reader([&] {
        auto snapshot = request->get_copy(key);
        got.store(snapshot.has_value());
    });
    const bool reached = pause->await();
    check("22_read_copy_paused_outside_lock", reached);
    check("23_close_can_complete_during_copy", request->close());
    pause->resume();
    reader.join();
    check("24_revoked_snapshot_does_not_escape", !got.load());
    check("25_read_copy_scope_inactive", !request->active());
    c->retire();
}

void replay_quota_and_retirement() {
    auto c = TaskCancellation::root();
    auto request = new_request({71, 8, 1}, 7, c);
    RequestLocalKeyV1<std::string> key;
    check("26_bind_first_mail", request->bind_original_mail_once(91));
    check("27_original_mail_immutable",
          !request->bind_original_mail_once(92) &&
          request->bound_to_original_mail(91));
    check("28_measured_value_boundary",
          request->set(key, std::string(1024, 'x')) &&
          request->used_bytes() == 1024);
    check("29_oversized_replacement_preserves_old",
          !request->set(key, std::string(1025, 'y')) &&
          request->get_copy(key) == std::string(1024, 'x') &&
          request->used_bytes() == 1024);
    c->retire();
    check("30_retired_authority_denies_mutation",
          !request->set(key, std::string("late")) &&
          !request->erase(key) && !request->active());
    request->close();
}
} // namespace

int main() {
    stop_and_sibling_survival();
    stop_wins_before_writer_commit();
    cancel_wins_before_writer_commit();
    close_wins_during_read_copy();
    replay_quota_and_retirement();
    if (passed != 30) return 2;
    std::puts("REQUEST_STOP_CANCEL_30_HOST_CASES_PASS");
}
