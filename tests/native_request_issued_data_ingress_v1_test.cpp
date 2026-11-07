#include "oreslang/runtime/NativeRequestIngressIssuerV1.hpp"

#include <array>
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
constexpr int per_actor = 16;
int checks = 0;
void check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "ISSUED_DATA_INGRESS_FAILED %s\n", label);
        std::abort();
    }
    std::printf("ISSUED_DATA_INGRESS_PASS %s\n", label);
    ++checks;
}
struct World {
    OresNativeActorScheduler* scheduler = nullptr;
    Registry::Ptr registry;
    Issuer::Ptr issuer;
    OresNativeActorRef actors[2]{};
    TaskCancellation::Ptr cancellations[2]{
        TaskCancellation::root(), TaskCancellation::root()};
    std::array<std::array<std::unique_ptr<Gate>, per_actor>, 2> gates{};
    RequestLocalKeyV1<int> key;
    std::mutex mutex;
    std::condition_variable cv;
    int completed[2]{0, 0};
    bool duplicate_denied[2][per_actor]{};
    bool foreign_denied[2][per_actor]{};
    bool cancelled_turn_denied = false;

    static void on_message(void* opaque, std::size_t actor_index,
                           std::uint64_t payload) {
        auto& w = *static_cast<World*>(opaque);
        assert(actor_index < 2);
        assert(payload >= 1 && payload <= per_actor);
        const auto actor = w.actors[actor_index];
        std::uint64_t actual_mail = 0;
        assert(ores_native_actor_scheduler_current_data_mail_sequence(
            w.scheduler, actor, &actual_mail));
        assert(actual_mail == payload);
        if (w.cancellations[actor_index]->is_cancelled()) {
            assert(!w.issuer->admit_current_data_turn(
                actor, w.cancellations[actor_index]));
            std::lock_guard<std::mutex> lock(w.mutex);
            w.cancelled_turn_denied = true;
            ++w.completed[actor_index];
            w.cv.notify_all();
            return;
        }
        auto gate = w.issuer->admit_current_data_turn(
            actor, w.cancellations[actor_index]);
        assert(gate && gate->authorized());
        // The authenticated same DATA mail cannot mint a second request.
        const bool duplicate_denied =
            !w.issuer->admit_current_data_turn(
                actor, w.cancellations[actor_index]);
        // Even from a real DATA carrier, a foreign actor is never issued
        // an identity or a scope.
        const bool foreign_denied =
            !w.issuer->admit_current_data_turn(
                w.actors[1-actor_index], w.cancellations[1-actor_index]);
        assert(gate->set(w.key, int(payload)));
        assert(gate->get_copy(w.key) == int(payload));
        // No request ownership survives a different actor's DATA.
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            assert(!w.gates[actor_index][payload - 1]);
            w.gates[actor_index][payload - 1] = std::move(gate);
            w.duplicate_denied[actor_index][payload - 1] = duplicate_denied;
            w.foreign_denied[actor_index][payload - 1] = foreign_denied;
            ++w.completed[actor_index];
        }
        w.cv.notify_all();
    }

    World() {
        scheduler = ores_native_actor_scheduler_open(
            2, 8, 2, &World::on_message, this);
        assert(scheduler);
        registry = Registry::create(scheduler, 2);
        assert(registry);
        issuer = Issuer::create(scheduler, registry.get(), 11, 22);
        assert(issuer);
        actors[0] = ores_native_actor_scheduler_spawn(scheduler, 0);
        actors[1] = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(actors[0].scheduler_domain && actors[1].scheduler_domain);
    }
    ~World() {
        assert(registry->shutdown_after_retire());
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
        for (auto& cancellation : cancellations)
            cancellation->retire();
        issuer.reset();
        registry.reset();
    }
    void deliver(std::size_t actor_index, std::uint64_t payload) {
        assert(ores_native_actor_scheduler_try_send(
            scheduler, actors[actor_index], payload)
            == ORES_NATIVE_SEND_ACCEPTED);
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return completed[actor_index] >= static_cast<int>(payload);
        }));
    }
};

void test_offturn_and_issuer_configuration() {
    World w;
    check("01_null_scheduler_issuer",
          !Issuer::create(nullptr, w.registry.get(), 11, 22));
    check("02_null_registry_issuer",
          !Issuer::create(w.scheduler, nullptr, 11, 22));
    check("03_zero_tenant_denied",
          !Issuer::create(w.scheduler, w.registry.get(), 0, 22));
    check("04_zero_isolate_denied",
          !Issuer::create(w.scheduler, w.registry.get(), 11, 0));
    auto* other = ores_native_actor_scheduler_open(
        2, 8, 1, &World::on_message, &w);
    assert(other);
    check("05_foreign_scheduler_denied",
          !Issuer::create(other, w.registry.get(), 11, 22));
    ores_native_actor_scheduler_retire(other);
    check("06_foreign_scheduler_drained",
          ores_native_actor_scheduler_drain(other));
    check("07_foreign_scheduler_destroyed",
          ores_native_actor_scheduler_try_destroy(other));
    check("08_offturn_actor_A_rejected",
          !w.issuer->admit_current_data_turn(
              w.actors[0], w.cancellations[0]));
    check("09_offturn_actor_B_rejected",
          !w.issuer->admit_current_data_turn(
              w.actors[1], w.cancellations[1]));
    OresNativeActorRef stale = w.actors[0];
    ++stale.generation;
    check("10_stale_generation_offturn_denied",
          !w.issuer->admit_current_data_turn(stale, w.cancellations[0]));
    check("11_empty_task_authority_denied",
          !w.issuer->admit_current_data_turn(w.actors[0], {}));
    check("12_issuer_counter_initially_zero",
          w.issuer->issued_request_count() == 0);
    check("13_original_actor_A_remains_ready",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[0]));
    check("14_original_actor_B_remains_ready",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[1]));
    check("issuer_duplicate_creation_DENIED",
          !Issuer::create(w.scheduler, w.registry.get(), 11, 22));
}

void test_issued_turns_and_revocation() {
    World w;
    char name[100];
    for (int index = 1; index <= per_actor; ++index) {
        for (std::size_t actor = 0; actor < 2; ++actor) {
            w.deliver(actor, static_cast<std::uint64_t>(index));
            std::snprintf(name, sizeof(name),
                          "mail_%02d_actor_%zu_registered", index, actor);
            check(name, bool(w.gates[actor][index-1]));
            std::snprintf(name, sizeof(name),
                          "mail_%02d_actor_%zu_inaccessible_offturn",
                          index, actor);
            check(name, !w.gates[actor][index-1]->authorized());
            std::snprintf(name, sizeof(name),
                          "mail_%02d_actor_%zu_read_denied_offturn",
                          index, actor);
            check(name, !w.gates[actor][index-1]->get_copy(w.key));
            std::snprintf(name, sizeof(name),
                          "mail_%02d_actor_%zu_duplicate_DENIED",
                          index, actor);
            check(name, w.duplicate_denied[actor][index-1]);
            std::snprintf(name, sizeof(name),
                          "mail_%02d_actor_%zu_cross_actor_DENIED",
                          index, actor);
            check(name, w.foreign_denied[actor][index-1]);
        }
        std::snprintf(name, sizeof(name),
                      "mail_%02d_unique_minted_request_ids", index);
        check(name, w.issuer->issued_request_count() ==
                    static_cast<std::uint64_t>(index * 2));
    }
    check("15_quota_safely_reached_32_requests",
          w.issuer->issued_request_count() == 32);
    check("16_both_actors_ready_before_STOP",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[0])
          && ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[1]));
    check("17_normal_STOP_actor_A",
          w.registry->signal_supervisor_termination(
              w.actors[0], Registry::SupervisorCause::normal_stop));
    check("18_actor_A_reaped_and_quota_returned",
          w.registry->wait_reaped(w.actors[0]));
    check("19_actor_B_still_ready",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[1]));
    check("20_actor_B_still_live_after_sibling_STOP",
          w.registry->reaped_actors() == 1);
    for (int i = 0; i < per_actor; ++i) {
        std::snprintf(name, sizeof(name), "stale_A_%02d_DENIED", i);
        check(name, !w.gates[0][i]->authorized() &&
              !w.gates[0][i]->set(w.key, 900) &&
              !w.gates[0][i]->get_copy(w.key));
    }
    check("21_panic_STOP_actor_B",
          w.registry->signal_supervisor_termination(
              w.actors[1], Registry::SupervisorCause::panic));
    check("22_actor_B_quiesced",
          w.registry->wait_reaped(w.actors[1]));
    check("23_both_actor_slots_independently_reaped",
          w.registry->reaped_actors() == 2);
    for (int i = 0; i < per_actor; ++i) {
        std::snprintf(name, sizeof(name), "stale_B_%02d_DENIED", i);
        check(name, !w.gates[1][i]->authorized() &&
              !w.gates[1][i]->get_copy(w.key));
    }
    check("24_generation_revival_denied",
          !ores_native_actor_scheduler_spawn(w.scheduler, 0).scheduler_domain);
    check("25_post_STOP_raw_turn_denied",
          !w.issuer->admit_current_data_turn(
              w.actors[0], w.cancellations[0]));
}
void test_cancelled_authority_on_real_data_turn() {
    World w;
    check("26_cancel_B_before_DATA",
          w.cancellations[1]->cancel("http request parent cancelled"));
    w.deliver(1, 1);
    check("27_cancelled_turn_no_gate",
          w.cancelled_turn_denied && !w.gates[1][0]);
    check("28_cancelled_turn_burns_minted_id",
          w.issuer->issued_request_count() == 1);
    check("29_cancelled_actor_still_scheduled",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[1]));
    check("30_no_offturn_retry_after_failure",
          !w.issuer->admit_current_data_turn(
              w.actors[1], w.cancellations[1]));
    check("31_live_sibling_after_cancelled_request",
          ores_native_actor_scheduler_poll_ready(w.scheduler, w.actors[0]));
}
// Scope is strongly retained by the DATA gate even after its scheduler
// is destroyed. The registry's STOP/retire cleanup must revoke it first, so
// every public access short-circuits before touching the stale raw pointer.
void test_terminated_gate_outlives_scheduler() {
    std::unique_ptr<Gate> escaped;
    {
        World w;
        w.deliver(0, 1);
        escaped = std::move(w.gates[0][0]);
        check("stale_gate_01_issued_before_destroy",
              bool(escaped));
        // World tears down the registry/actors/scheduler in this scope.
    }
    RequestLocalKeyV1<int> unrelated;
    check("stale_gate_02_authorized_denied_after_destroy",
          escaped && !escaped->authorized());
    check("stale_gate_03_copy_denied_after_destroy",
          escaped && !escaped->get_copy(unrelated));
    check("stale_gate_04_mutations_denied_after_destroy",
          escaped && !escaped->set(unrelated, 9)
          && !escaped->remove(unrelated));
}

} // namespace

int main() {
    test_offturn_and_issuer_configuration();
    test_issued_turns_and_revocation();
    test_cancelled_authority_on_real_data_turn();
    test_terminated_gate_outlives_scheduler();
    // Fixed: 14 bootstrap + 25 retirement + 31 final? Count by markers
    // is runtime validated by the workflow, never infer an execution pass
    // from this source comment.
    std::printf("NATIVE_ISSUED_DATA_INGRESS_HOST_PASS total=%d\n", checks);
}
