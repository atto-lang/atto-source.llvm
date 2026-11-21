// Host-only scheduler DATA-turn admission-versus-STOP model. The test
// intentionally parks a carrier within a test callback; production ingress
// must never park a carrier to await cleanup or actor termination.
#include "oreslang/runtime/NativeRequestIngressIssuerV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

using namespace oreslang::runtime;
namespace {
using Registry = RequestLocalActorTerminationRegistryV1;
using Issuer = NativeRequestIngressIssuerV1;
using Gate = NativeRequestDataMailGateV1;
int checked = 0;
void check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "ISSUED_DATA_STOP_RACE_FAILED %s\n", label);
        std::abort();
    }
    std::printf("ISSUED_DATA_STOP_RACE_PASS %s\n", label);
    ++checked;
}
struct Race {
    OresNativeActorScheduler* scheduler = nullptr;
    Registry::Ptr registry;
    Issuer::Ptr issuer;
    OresNativeActorRef a{}, b{};
    TaskCancellation::Ptr cancellation{TaskCancellation::root()};
    RequestLocalKeyV1<int> key;
    std::unique_ptr<Gate> issued_gate;
    std::mutex mutex;
    std::condition_variable cv;
    bool reached = false;
    bool proceed = false;
    bool finished = false;
    bool gate_issued_before_stop = false;
    bool late_admission_denied = false;
    const bool before_stop;

    explicit Race(bool issue_before_stop) : before_stop(issue_before_stop) {
        scheduler = ores_native_actor_scheduler_open(
            2, 8, 2, &Race::on_message, this);
        assert(scheduler);
        registry = Registry::create(scheduler, 2);
        assert(registry);
        issuer = Issuer::create(scheduler, registry.get(), 15, 16);
        assert(issuer);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        b = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~Race() {
        assert(registry->shutdown_after_retire());
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
        cancellation->retire();
        issued_gate.reset();
        issuer.reset();
        registry.reset();
    }

    static void on_message(void* opaque, std::size_t actor_index,
                           std::uint64_t payload) {
        auto& r = *static_cast<Race*>(opaque);
        assert(actor_index == 0 && payload == 1);
        if (r.before_stop) {
            r.issued_gate = r.issuer->admit_current_data_turn(
                r.a, r.cancellation);
            assert(r.issued_gate && r.issued_gate->authorized());
            assert(r.issued_gate->set(r.key, 71));
        }
        {
            std::unique_lock<std::mutex> guard(r.mutex);
            r.gate_issued_before_stop = bool(r.issued_gate);
            r.reached = true;
            r.cv.notify_all();
            const bool released = r.cv.wait_for(
                guard, std::chrono::seconds(8),
                [&] { return r.proceed; });
            assert(released);
        }
        // Physical callback resumes even if actor STOP has already fenced
        // its logical request. It must never mint a new request after STOP.
        if (!r.before_stop) {
            auto late = r.issuer->admit_current_data_turn(
                r.a, r.cancellation);
            r.late_admission_denied = !late;
        } else {
            r.late_admission_denied = !r.issued_gate->authorized() &&
                !r.issued_gate->set(r.key, 99) &&
                !r.issued_gate->get_copy(r.key);
        }
        {
            std::lock_guard<std::mutex> guard(r.mutex);
            r.finished = true;
        }
        r.cv.notify_all();
    }

    void wait_at_barrier() {
        assert(ores_native_actor_scheduler_try_send(scheduler, a, 1) ==
               ORES_NATIVE_SEND_ACCEPTED);
        std::unique_lock<std::mutex> guard(mutex);
        assert(cv.wait_for(guard, std::chrono::seconds(8),
                           [&] { return reached; }));
    }
    void release_barrier() {
        {
            std::lock_guard<std::mutex> guard(mutex);
            proceed = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> guard(mutex);
        assert(cv.wait_for(guard, std::chrono::seconds(8),
                           [&] { return finished; }));
    }
};
void run_stop_vs_data_issuance(bool issue_before_stop) {
    for (int trial = 0; trial < 30; ++trial) {
        Race r(issue_before_stop);
        r.wait_at_barrier();
        char label[100];
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_barrier_entered",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, r.reached &&
              r.gate_issued_before_stop == issue_before_stop);
        const bool stopped = r.registry->signal_supervisor_termination(
            r.a, Registry::SupervisorCause::cancellation);
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_STOP_returns_without_carrier_wait",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, stopped &&
              ores_native_actor_scheduler_poll_ready(r.scheduler, r.b));
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_NOT_done_while_DATA_active",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, !r.registry->poll_reaped(r.a));
        if (issue_before_stop) {
            std::snprintf(label, sizeof(label),
                          "race_issued_%02d_immediate_request_revoke", trial);
            check(label, r.issued_gate && !r.issued_gate->authorized());
        }
        r.release_barrier();
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_no_new_read_or_write_after_STOP",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, r.late_admission_denied);
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_quiescent_reap",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, r.registry->wait_reaped(r.a) &&
              r.registry->reaped_actors() == 1);
        std::snprintf(label, sizeof(label),
                      "race_%s_%02d_sibling_survives",
                      issue_before_stop ? "issued" : "unissued", trial);
        check(label, ores_native_actor_scheduler_poll_ready(r.scheduler, r.b));
    }
}
} // namespace

int main() {
    run_stop_vs_data_issuance(false);
    run_stop_vs_data_issuance(true);
    check("final_390_real_carrier_STOP_race_assertions", checked == 390);
    std::printf("NATIVE_ISSUED_DATA_STOP_RACE_HOST_PASS total=%d trials=60\n",
                checked);
}
