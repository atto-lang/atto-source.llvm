#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSupervisorPanicBridge.hpp"
#include "oreslang/runtime/NativePhysicalStopEvidence.hpp"
#include "oreslang/runtime/SourceSupervisorStop.hpp"
#include "oreslang/runtime/SupervisorControlScheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void require(bool value, const char* msg) {
    if (!value) throw std::runtime_error(msg);
}

struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef parent{}, child{};
    std::shared_ptr<SupervisorControlScheduler> control;
    std::shared_ptr<SupervisorChildRegistry> registry;
    std::shared_ptr<NativePhysicalStopEvidence> host_receipts;
    std::shared_ptr<SupervisorProcess> process;
    std::unique_ptr<NativeSupervisorPanicBridge> bridge;
    std::atomic<unsigned> terminal_events{0};
    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> child_messages{0};
    std::atomic<bool> child_cleanup_done{false};
    std::atomic<unsigned> applied{0};
    std::atomic<unsigned> failed_checks{0};
    std::atomic<std::uint64_t> observed_child{0};
    std::atomic<std::uint64_t> observed_incarnation{0};
};

static void ordinary(void* raw, size_t slot, std::uint64_t payload) {
    auto& f = *static_cast<Fixture*>(raw);
    if (slot != f.child.actor_slot || payload != 99) {
        ++f.failed_checks;
        return;
    }
    ++f.child_messages;
    // Publish while still on the dying child's carrier, as happens during
    // actual terminal actor handling. Parent CONTROL must wait until this
    // physical callback has returned before invoking the source STOP policy.
    f.child_cleanup_done.store(true);
    if (!ores_native_actor_scheduler_stop(f.scheduler, f.child)
        || !ores_native_actor_scheduler_publish_failure(
            f.scheduler, f.parent,
            OresNativeActorTerminalFailure{
                ORES_NATIVE_CONTROL_PANIC, f.child, 17, 91}))
        ++f.failed_checks;
}

static void typed_terminal(void* raw, size_t parent_slot,
                           OresNativeActorTerminalFailure event) {
    auto& f = *static_cast<Fixture*>(raw);
    ++f.terminal_events;
    if (f.bridge && f.bridge->on_panic(parent_slot, event))
        ++f.accepted;
    else
        ++f.failed_checks;
}

static void exercise(bool ambiguous, bool lost_ack = false,
                     bool missing_attestation = false) {
    require(!lost_ack || ambiguous, "lost ACK must model an unknown callback result");
    require(!missing_attestation || !ambiguous,
            "missing-attestation test requires normally returning callback");
    const bool unknown = ambiguous || missing_attestation;
    Fixture f;
    f.scheduler = ores_native_actor_scheduler_open_typed_control(
        2, 2, 2, ordinary, nullptr, typed_terminal, &f);
    require(f.scheduler != nullptr, "failed to create real typed scheduler");
    f.parent = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    f.child = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    require(f.parent.scheduler_domain != 0 && f.child.generation != 0,
            "native scheduler did not start actor incarnations");

    constexpr std::uint64_t supervisor = 6144;
    const auto domain = f.parent.scheduler_domain;
    f.control = std::make_shared<SupervisorControlScheduler>(2, 16);
    f.registry = std::make_shared<SupervisorChildRegistry>(supervisor, domain);
    f.host_receipts = std::make_shared<NativePhysicalStopEvidence>(
        f.scheduler, f.registry);
    constexpr auto source = R"(
        define supervisor Root as
          on_panic(f: Failure): void {
            self.stop(f.child);
            return;
          }
        end
    )";
    f.process = install_source_supervisor_stop_verified(
        source, supervisor, domain, 3, f.control->adapter(), f.registry,
        [&](const SupervisorProcess::Resolution& resolution) {
            const auto& n = resolution.notice;
            f.observed_child.store(n.logical_child_id);
            f.observed_incarnation.store(n.logical_incarnation);
            ++f.applied;
            try {
                const SupervisorChildRegistry::ChildRef child{
                    supervisor, domain, n.logical_child_id};
                const auto state = f.registry->snapshot(child);
                require(state.phase == SupervisorChildRegistry::Phase::stopping,
                        "host effect ran before exclusive STOP reservation");
                require(state.actor.actor_slot == f.child.actor_slot
                        && state.actor.generation == f.child.generation
                        && state.incarnation == n.logical_incarnation,
                        "typed terminal failure switched actor incarnations");
                require(f.child_cleanup_done.load()
                        && ores_native_actor_scheduler_poll_done(f.scheduler, f.child),
                        "policy ran before child cleanup and physical finalization");
                bool restart_denied = false;
                try {
                    (void) f.registry->begin_restart(child, f.child, n.logical_incarnation);
                } catch (const std::logic_error&) {
                    restart_denied = true;
                }
                require(restart_denied, "RESTART raced while STOP effect active");
            } catch (...) {
                ++f.failed_checks;
                throw;
            }
            // This receipt is written by the trusted physical lifecycle
            // adapter, using the real scheduler DONE barrier. It is not a
            // fabricated callback result and not evidence of other effects.
            // The lost-ACK case records DONE before throwing to CONTROL.
            if ((!ambiguous && !missing_attestation) || lost_ack) {
                const auto current = f.registry->stop_receipt(
                    SupervisorChildRegistry::ChildRef{
                        supervisor, domain, n.logical_child_id});
                // Even physical DONE cannot turn a forged, foreign
                // reservation structure into a host-issued receipt.
                auto forged = current;
                forged.turn_generation++;
                bool forged_rejected = false;
                try { (void) f.host_receipts->observe_physical_done(forged); }
                catch (const std::logic_error&) { forged_rejected = true; }
                require(forged_rejected,
                        "host ledger accepted a caller-forged STOP reservation");
                const auto evidence =
                    f.host_receipts->observe_physical_done(current);
                bool replay_rejected = false;
                try { (void) f.host_receipts->observe_physical_done(current); }
                catch (const std::logic_error&) { replay_rejected = true; }
                require(replay_rejected,
                        "host ledger accepted duplicate physical DONE evidence");
                require(evidence.control_turn == resolution.turn_generation
                        && evidence.incarnation == n.logical_incarnation
                        && evidence.actor.generation == n.generation,
                        "host DONE attestation bound to wrong STOP incarnation");
            }
            if (ambiguous)
                throw std::runtime_error("unknown external STOP acknowledgment");
        }, f.host_receipts);
    f.bridge = std::make_unique<NativeSupervisorPanicBridge>(
        f.scheduler, f.parent, f.process, f.registry);
    require(f.bridge->trusted_child_registry() == f.registry,
            "panic bridge did not share the source installer's identity registry");
    f.bridge->register_child(f.child);
    require(f.registry->tracked_children() == 1,
            "physical child spawn was not registered with supervisor");

    require(ores_native_actor_scheduler_send(f.scheduler, f.child, 99),
            "native child message could not trigger terminal handling");

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f.applied.load() == 1
            && f.accepted.load() == 1 && f.terminal_events.load() == 1
            && (unknown ? f.process->undelivered() == 1
                          : f.process->resolved() == 1))
            break;
        std::this_thread::sleep_for(1ms);
    }
    require(f.child_messages == 1 && f.child_cleanup_done.load(),
            "terminal actor work did not complete its cleanup");
    require(f.terminal_events == 1 && f.accepted == 1,
            "typed native panic did not pass the bridge exactly once");
    require(f.applied == 1 && f.failed_checks == 0,
            "source STOP effect missing or failed runtime invariants");
    require(f.observed_child != 0 && f.observed_incarnation == 1,
            "host did not receive trusted first logical child generation");
    const auto logical = SupervisorChildRegistry::ChildRef{
        supervisor, domain, f.observed_child.load()};
    const auto state = f.registry->snapshot(logical);
    require(state.phase == (unknown
                ? SupervisorChildRegistry::Phase::stopping
                : SupervisorChildRegistry::Phase::stopped),
            "external STOP result did not produce correct terminal disposition");
    const auto receipt = f.registry->stop_receipt(logical);
    require(receipt.status == (unknown
                ? SupervisorChildRegistry::StopReceiptStatus::uncertain
                : SupervisorChildRegistry::StopReceiptStatus::acknowledged),
            "host STOP receipt did not preserve applied versus ambiguous outcome");
    require(receipt.turn_generation != 0
                && receipt.failed_actor.actor_slot == f.child.actor_slot
                && receipt.incarnation == 1 && receipt.host_invoked,
            "STOP host invocation not bound to the exact physical incarnation");

    // These journal entries come from the native scheduler -> PANIC bridge ->
    // compiled supervisor CONTROL -> actual host sink path. Do not fabricate
    // an applied STOP after the sink threw or assume it was applied.
    const auto observed = f.registry->committed_transitions();
    unsigned invokes = 0, reserves = 0, uncertain = 0, acknowledgments = 0;
    std::uint64_t reserved_turn = 0;
    for (const auto& event : observed) {
        const std::string action(event.operation);
        if (action == "stop.reserve") {
            ++reserves;
            reserved_turn = event.stop_turn;
        } else if (action == "stop.invoke-asserted") {
            ++invokes;
            require(event.stop_turn == reserved_turn && reserved_turn != 0,
                    "host entry was not linked to the original STOP turn");
        } else if (action == "stop.uncertain") {
            ++uncertain;
        } else if (action == "stop.ack-applied") {
            ++acknowledgments;
        }
    }
    require(reserves == 1 && invokes == 1
                && uncertain == (unknown ? 1U : 0U)
                && acknowledgments == (unknown ? 0U : 1U),
            "real CONTROL host STOP was duplicated or falsely acknowledged");
    std::printf("NATIVE_SUPERVISOR_STOP_OBSERVED_V1=%s reserved=1 invoked=1 "
                "uncertain=%u acknowledged=%u\n",
                unknown ? "uncertain" : "successful",
                uncertain, acknowledgments);
    require(f.process->resolved() == (unknown ? 0U : 1U)
                && f.process->undelivered() == (unknown ? 1U : 0U),
            "CONTROL misaccounted acknowledged or ambiguous STOP");
    bool restart_denied = false;
    try {
        (void) f.registry->begin_restart(logical, f.child, 1);
    } catch (const std::logic_error&) {
        restart_denied = true;
    }
    require(restart_denied, "STOP disposition reopened an unsafe RESTART");
    require(!f.bridge->on_panic(f.parent.actor_slot,
                OresNativeActorTerminalFailure{
                    ORES_NATIVE_CONTROL_PANIC, f.child, 17, 91}),
            "duplicate typed panic replayed a consumed child incarnation");

    // Prove that an uncertain STOP with no host evidence remains quarantined,
    // whereas a separately recorded physical DONE can reconcile after the
    // original host acknowledgment is lost, without replaying the sink.
    require(f.host_receipts->size() ==
                (unknown && !lost_ack ? 0U : 1U),
            "host-owned physical STOP evidence ledger has wrong cardinality");
    if (unknown) {
        const auto original = f.registry->stop_receipt(logical);
        auto forged = original;
        forged.turn_generation++;
        require(!f.host_receipts->confirms_physical_done(forged),
                "foreign CONTROL turn borrowed physical STOP proof");
        forged = original;
        forged.failed_actor.generation++;
        require(!f.host_receipts->confirms_physical_done(forged),
                "foreign physical generation borrowed physical STOP proof");
        forged = original;
        forged.child.logical_child_id++;
        require(!f.host_receipts->confirms_physical_done(forged),
                "foreign logical child borrowed physical STOP proof");
        const bool reconciled = f.registry->reconcile_uncertain_stop(
            logical, f.child, 1, original.turn_generation,
            [&](const SupervisorChildRegistry::StopReceipt& pending) {
                return f.host_receipts->confirms_physical_done(pending);
            });
        require(reconciled == lost_ack,
                "unknown STOP disposition did not follow independent host evidence");
        require(f.registry->snapshot(logical).phase ==
                    (lost_ack ? SupervisorChildRegistry::Phase::stopped
                              : SupervisorChildRegistry::Phase::stopping),
                "host ledger reconciliation broke STOP quarantine");
        require(f.applied.load() == 1,
                "reconciliation must never invoke original host callback twice");
        if (lost_ack) {
            require(f.registry->stop_receipt(logical).status ==
                        SupervisorChildRegistry::StopReceiptStatus::acknowledged,
                    "lost ACK did not reconcile one physically finalized child");
            std::puts("NATIVE_SUPERVISOR_LOST_ACK_RECONCILED_V1"
                      " attested=1 invoked=1 reconciled=1 replay=0");
        } else {
            std::puts("NATIVE_SUPERVISOR_UNKNOWN_QUARANTINED_V1"
                      " attested=0 invoked=1 reconciled=0 replay=0");
            if (missing_attestation)
                std::puts("NATIVE_SUPERVISOR_NO_PROOF_QUARANTINED_V1"
                          " callback_ok=1 attested=0 reconciled=0");
        }
    }

    // Reclaim only an acknowledged, exact-identity physical receipt.
    // Neither UNCERTAIN nor an absent attestation may be evicted early.
    if (!unknown || lost_ack) {
        const auto acknowledged = f.registry->stop_receipt(logical);
        require(f.host_receipts->forget_acknowledged(acknowledged)
                    && f.host_receipts->size() == 0,
                "acknowledged physical STOP evidence was not reclaimed");
        require(!f.host_receipts->confirms_physical_done(acknowledged),
                "retired attestation remained eligible for replay");
    } else {
        require(!f.host_receipts->forget_acknowledged(receipt),
                "unresolved STOP evidence was reclaimed without proof");
    }

    // The scheduler is a raw C handle. Revoke every observer before freeing it.
    f.host_receipts->close();
    f.host_receipts->close();
    require(f.host_receipts->size() == 0
                && !f.host_receipts->confirms_physical_done(receipt),
            "closed STOP ledger leaked proof or a scheduler pointer");
    bool closed_observer_denied = false;
    try { (void) f.host_receipts->observe_physical_done(receipt); }
    catch (const std::logic_error&) { closed_observer_denied = true; }
    require(closed_observer_denied, "closed STOP ledger accepted new evidence");

    require(ores_native_actor_scheduler_stop(f.scheduler, f.parent),
            "supervisor parent could not stop");
    ores_native_actor_scheduler_retire(f.scheduler);
    require(ores_native_actor_scheduler_drain(f.scheduler),
            "native scheduler did not drain CONTROL");
    require(ores_native_actor_scheduler_try_destroy(f.scheduler),
            "native scheduler could not destroy after drain");
    f.scheduler = nullptr;
    f.bridge.reset();
    f.process->close();
    f.control->close();
}

int main() {
    exercise(false);
    exercise(true);
    exercise(true, true);
    exercise(false, false, true);
    std::puts("NATIVE_SOURCE_SUPERVISOR_BRIDGE_E2E_PASS typed=1 stop=1 unknown_quarantined=1");
}
