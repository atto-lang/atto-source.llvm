#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativePhysicalStopEvidence.hpp"
#include "oreslang/runtime/SupervisorChildRegistry.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace oreslang::runtime;

static void check(bool yes, const char* msg) {
    if (!yes) throw std::runtime_error(msg);
}
static void no_op(void*, size_t, std::uint64_t) {}

int main() {
    // Real C scheduler: the host-only registry deliberately makes an incorrect
    // early FINALIZED claim to test independence from registry assertions.
    auto* scheduler = ores_native_actor_scheduler_open(
        2, 2, 2, no_op, nullptr);
    check(scheduler != nullptr, "physical actor scheduler failed");
    const auto actor = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain && actor.generation,
          "actor incarnation was not created");

    auto registry = std::make_shared<SupervisorChildRegistry>(
        879, actor.scheduler_domain);
    NativePhysicalStopEvidence evidence(scheduler, registry);
    const auto child = registry->register_child(actor);
    registry->finalized(child.child, actor, 1);
    registry->begin_stop(child.child, actor, 1, 7);
    registry->claim_host_stop_invocation(child.child, actor, 1, 7);
    auto receipt = registry->stop_receipt(child.child);
    check(receipt.host_invoked
              && receipt.status == SupervisorChildRegistry::StopReceiptStatus::pending,
          "the fake finalized host receipt fixture was not reserved");

    bool rejected_live = false;
    try { (void) evidence.observe_physical_done(receipt); }
    catch (const std::logic_error&) { rejected_live = true; }
    check(rejected_live && evidence.size() == 0,
          "live physical actor was falsely attested as stopped");

    check(ores_native_actor_scheduler_stop(scheduler, actor)
              && ores_native_actor_scheduler_wait_done(scheduler, actor),
          "real actor failed to reach physical DONE");

    auto forged = receipt;
    forged.child.logical_child_id++;
    bool rejected_foreign = false;
    try { (void) evidence.observe_physical_done(forged); }
    catch (const std::logic_error&) { rejected_foreign = true; }
    check(rejected_foreign && evidence.size() == 0,
          "foreign registry child forged a physical STOP receipt");

    const auto issued = evidence.observe_physical_done(receipt);
    check(issued.evidence_sequence == 1
              && issued.control_turn == 7
              && issued.actor.generation == actor.generation
              && issued.logical_child_id == child.child.logical_child_id
              && evidence.confirms_physical_done(receipt),
          "host receipt was not bound to the physical DONE and exact turn");

    bool rejected_duplicate = false;
    try { (void) evidence.observe_physical_done(receipt); }
    catch (const std::logic_error&) { rejected_duplicate = true; }
    check(rejected_duplicate && evidence.size() == 1,
          "host ledger recorded duplicate attestation");

    registry->record_stop_uncertain(child.child, actor, 1, 7);
    check(registry->reconcile_uncertain_stop(child.child, actor, 1, 7,
              [&](const SupervisorChildRegistry::StopReceipt& original) {
                  return evidence.confirms_physical_done(original);
              }), "verified physical STOP could not recover lost acknowledgment");
    check(registry->snapshot(child.child).phase
              == SupervisorChildRegistry::Phase::stopped,
          "reconciliation did not commit stopped child");
    check(!evidence.confirms_physical_done(receipt),
          "acknowledged STOP was incorrectly left eligible for reconciliation");

    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler)
              && ores_native_actor_scheduler_try_destroy(scheduler),
          "native scheduler did not physically drain before destruction");
    std::puts("NATIVE_PHYSICAL_STOP_EVIDENCE_TEST_PASS live_denied=1 forged_denied=1 duplicate_denied=1 reconciled=1");
}
