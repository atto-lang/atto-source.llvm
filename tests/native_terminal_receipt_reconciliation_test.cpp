#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSupervisorPanicBridge.hpp"
#include "oreslang/runtime/SupervisorControlScheduler.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace oreslang::runtime;
using namespace std::chrono_literals;

static void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static void data(void*, OresNativeActorRef, std::uint64_t) {}
static void panic(void*, OresNativeActorRef,
                  OresNativeActorTerminalFailure) {}
static void expect_done(OresNativeActorScheduler* s, OresNativeActorRef a) {
    require(ores_native_actor_scheduler_stop(s, a)
                && ores_native_actor_scheduler_wait_done(s, a),
            "physical actor did not finish");
}
static void shutdown(OresNativeActorScheduler* scheduler) {
    ores_native_actor_scheduler_retire(scheduler);
    require(ores_native_actor_scheduler_drain(scheduler)
                && ores_native_actor_scheduler_try_destroy(scheduler),
            "native scheduler could not shut down");
}
int main() {
    for (int reject_host = 0; reject_host < 2; ++reject_host) {
        auto* scheduler = ores_native_actor_scheduler_open_identity_control(
            2, 2, 2, data, nullptr, panic, nullptr);
        require(scheduler != nullptr, "identity scheduler unavailable");
        auto parent = ores_native_actor_scheduler_spawn(scheduler, 0);
        auto child = ores_native_actor_scheduler_spawn(scheduler, 1);
        auto control = std::make_shared<SupervisorControlScheduler>(1, 16);
        auto registry = std::make_shared<SupervisorChildRegistry>(
            7331, parent.scheduler_domain);
        std::atomic<unsigned> policy_calls{0}, effects{0};
        auto process = std::make_shared<SupervisorProcess>(
            7331, parent.scheduler_domain, 4,
            reject_host ? SupervisorProcess::Enqueue(
                [](SupervisorControlScheduler::Task) { return false; })
                        : control->adapter(),
            [&](const SupervisorFailureNotice& notice)
                    -> std::optional<SupervisorProcess::Directive> {
                if (notice.logical_child_id == 0 || notice.logical_incarnation != 1
                        || notice.generation != child.generation)
                    throw std::runtime_error("reconciled notice lost exact identity");
                ++policy_calls;
                return SupervisorProcess::Directive::acknowledge;
            },
            [&](const SupervisorProcess::Resolution&) { ++effects; });
        auto bridge = std::make_unique<NativeSupervisorPanicBridge>(
            scheduler, parent, process, registry);
        bridge->register_child(child);
        const auto before = registry->tracked_children();
        expect_done(scheduler, child);
        expect_done(scheduler, parent);
        OresNativeActorTerminalFailure failure{
            ORES_NATIVE_CONTROL_PANIC, child, 9201, 443
        };
        require(!ores_native_actor_scheduler_publish_failure(
                    scheduler, parent, failure),
                "retired parent accepted terminal PANIC");
        require(!ores_native_actor_scheduler_publish_failure(
                    scheduler, parent, OresNativeActorTerminalFailure{
                        ORES_NATIVE_CONTROL_PANIC, child, 9000, 444}),
                "second terminal attempt bypassed one-shot identity");
        OresNativeTerminalReceipt receipt{};
        // STOP won BEFORE ordinary CONTROL callback authorization. The
        // original-parent claim MUST fail without consuming RECORDED evidence,
        // even though the reconciler may later claim that same exact receipt.
        require(!ores_native_actor_scheduler_claim_live_parent_terminal_receipt(
                    scheduler, parent, child, &receipt),
                "stopped original parent claimed a new host CONTROL effect");
        require(ores_native_actor_scheduler_terminal_receipt(
                    scheduler, parent, child, &receipt)
                    && receipt.status == ORES_NATIVE_TERMINAL_RECEIPT_RECORDED
                    && receipt.was_published_to_control == 0
                    && receipt.failure.kind == ORES_NATIVE_CONTROL_PANIC
                    && receipt.failure.failure_code == failure.failure_code
                    && receipt.failure.correlation == failure.correlation,
                "original terminal envelope lost across parent STOP");
        require(!ores_native_actor_scheduler_terminal_receipt(
                    scheduler, OresNativeActorRef{
                        parent.scheduler_domain, parent.actor_slot,
                        parent.generation + 1}, child, &receipt),
                "foreign parent generation queried old receipt");
        require(!ores_native_actor_scheduler_seal_normal_exit(scheduler, child)
                    && !ores_native_actor_scheduler_recycle_normal_done(
                        scheduler, child).scheduler_domain,
                "unreconciled PANIC entered normal-exit recycler");
        require(!bridge->on_panic(parent, failure)
                    && bridge->registered_children() == before,
                "stopped parent callback consumed terminal registration");
        require(ores_native_actor_scheduler_seal_normal_exit(
                    scheduler, parent), "normal parent could not be sealed");
        auto recovered = ores_native_actor_scheduler_recycle_normal_done(
            scheduler, parent);
        require(recovered.generation == parent.generation + 1
                    && recovered.actor_slot == parent.actor_slot,
                "parent slot did not advance its generation");
        OresNativeRestartLease lease{};
        require(ores_native_actor_scheduler_acquire_restart_lease(
                    scheduler, recovered, &lease)
                    && ores_native_actor_scheduler_publish_restart_lease(
                        scheduler, lease)
                    && ores_native_actor_scheduler_release_restart_lease(
                        scheduler, lease),
                "recovered parent not physically published");
        bool threw = false;
        try {
            (void) bridge->reconcile_quarantined_terminal(recovered, child);
        } catch (const std::logic_error&) {
            threw = true;
        }
        require(threw == (reject_host != 0),
                "host enqueue failure/reconciliation outcome not fenced");
        require(ores_native_actor_scheduler_terminal_receipt(
                    scheduler, parent, child, &receipt)
                    && receipt.status == (reject_host
                        ? ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                        : ORES_NATIVE_TERMINAL_RECEIPT_ACKNOWLEDGED)
                    && receipt.nonce != 0,
                "terminal receipt did not retain exact claim status");
        require(!ores_native_actor_scheduler_claim_terminal_receipt(
                    scheduler, parent, child, &receipt),
                "duplicate reconciler claimed a completed/uncertain effect");
        require(bridge->registered_children() == 0
                    && registry->snapshot(registry->committed_transitions()
                        .front().state->child).phase
                           == SupervisorChildRegistry::Phase::finalized,
                "logical child was not finalized exactly once");
        try {
            (void) bridge->reconcile_quarantined_terminal(recovered, child);
            threw = false;
        } catch (const std::logic_error&) {
            threw = true;
        }
        require(threw, "reconciliation replay accepted stale logical child");
        if (!reject_host) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (effects.load() == 0 &&
                    std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            require(policy_calls.load() == 1 && effects.load() == 1
                        && bridge->accepted() == 1,
                    "exact terminal host effect did not execute once");
        } else {
            require(policy_calls.load() == 0 && effects.load() == 0,
                    "failed CONTROL dispatch nevertheless ran host effect");
        }
        // A forged acknowledgement may not convert UNCERTAIN into SUCCESS.
        auto forged = receipt;
        forged.nonce += 1;
        forged.status = ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED;
        require(!ores_native_actor_scheduler_ack_terminal_receipt(
                    scheduler, forged), "forged receipt nonce acknowledged");
        require(!ores_native_actor_scheduler_recycle_normal_done(
                    scheduler, child).scheduler_domain,
                "PANIC incarnation recycled after receipt reconciliation");
        expect_done(scheduler, recovered);
        bridge.reset();
        process->close();
        control->close();
        shutdown(scheduler);
    }
    // Concurrent reconcilers may all inspect the same pre-delivery receipt,
    // but exactly one can transition it to CLAIMED under the scheduler lock.
    // No mutable receipt token is shared by the racing host threads.
    for (int iteration = 0; iteration < 20; ++iteration) {
        auto* scheduler = ores_native_actor_scheduler_open_identity_control(
            2, 2, 2, data, nullptr, panic, nullptr);
        require(scheduler != nullptr, "concurrent claim scheduler unavailable");
        auto parent = ores_native_actor_scheduler_spawn(scheduler, 0);
        auto child = ores_native_actor_scheduler_spawn(scheduler, 1);
        expect_done(scheduler, child);
        expect_done(scheduler, parent);
        const auto failure = OresNativeActorTerminalFailure{
            ORES_NATIVE_CONTROL_PANIC, child, 932, 211
        };
        require(!ores_native_actor_scheduler_publish_failure(
                    scheduler, parent, failure),
                "STOPped parent admitted concurrent PANIC");
        std::atomic<bool> start{false};
        std::atomic<int> claimed{0};
        std::vector<std::thread> threads;
        std::vector<OresNativeTerminalReceipt> claims(8);
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&, i] {
                while (!start.load()) std::this_thread::yield();
                if (ores_native_actor_scheduler_claim_terminal_receipt(
                        scheduler, parent, child, &claims[i]))
                    claimed.fetch_add(1);
            });
        }
        start.store(true);
        for (auto& thread : threads) thread.join();
        require(claimed.load() == 1,
                "two concurrent reconcilers claimed one terminal host effect");
        OresNativeTerminalReceipt receipt{};
        require(ores_native_actor_scheduler_terminal_receipt(
                    scheduler, parent, child, &receipt)
                    && receipt.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                    && receipt.nonce,
                "one-use receipt claim was not retained after race");
        for (const auto& attempt : claims)
            if (attempt.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED)
                require(attempt.nonce == receipt.nonce,
                        "claim returned a foreign or inconsistent nonce");
        require(!ores_native_actor_scheduler_seal_normal_exit(scheduler, child),
                "claimed PANIC was reclassified as normal exit");
        shutdown(scheduler);
    }
    // Exercise the real scheduler mutex as the linearization point for a
    // concurrently arriving STOP and ordinary parent callback claim. Exactly
    // one wins. A failed ordinary claim keeps an unconsumed RECORDED receipt
    // eligible for trusted recovery, never an ambiguous automatic replay.
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        auto* scheduler = ores_native_actor_scheduler_open_identity_control(
            2, 2, 2, data, nullptr, panic, nullptr);
        require(scheduler != nullptr, "race scheduler unavailable");
        const auto parent = ores_native_actor_scheduler_spawn(scheduler, 0);
        const auto child = ores_native_actor_scheduler_spawn(scheduler, 1);
        expect_done(scheduler, child);
        const OresNativeActorTerminalFailure failure{
            ORES_NATIVE_CONTROL_PANIC, child, 9302, 544
        };
        require(ores_native_actor_scheduler_publish_failure(
                    scheduler, parent, failure),
                "race fixture did not publish terminal envelope");
        std::atomic<bool> start{false}, claim_ok{false}, stop_ok{false};
        OresNativeTerminalReceipt claimed{};
        std::thread claimant([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            claim_ok.store(
                ores_native_actor_scheduler_claim_live_parent_terminal_receipt(
                    scheduler, parent, child, &claimed) != 0);
        });
        std::thread stopper([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            stop_ok.store(ores_native_actor_scheduler_stop(scheduler, parent) != 0);
        });
        start.store(true, std::memory_order_release);
        claimant.join();
        stopper.join();
        require(stop_ok.load(), "concurrent original parent STOP failed");
        require(ores_native_actor_scheduler_wait_done(scheduler, parent),
                "concurrent parent did not become physical DONE");
        OresNativeTerminalReceipt current{};
        require(ores_native_actor_scheduler_terminal_receipt(
                    scheduler, parent, child, &current),
                "raced STOP erased exact terminal receipt");
        require(!ores_native_actor_scheduler_claim_live_parent_terminal_receipt(
                    scheduler, parent, child, &claimed),
                "ordinary claim crossed post-STOP parent fence");
        if (claim_ok.load()) {
            require(current.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                        && claimed.nonce != 0
                        && ores_native_actor_scheduler_ack_terminal_receipt(
                            scheduler, claimed),
                    "pre-STOP claim did not retain one-use authority");
        } else {
            require(current.status == ORES_NATIVE_TERMINAL_RECEIPT_RECORDED,
                    "STOP-first race consumed unclaimed receipt");
            require(ores_native_actor_scheduler_claim_terminal_receipt(
                        scheduler, parent, child, &claimed)
                        && claimed.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                        && ores_native_actor_scheduler_ack_terminal_receipt(
                            scheduler, claimed),
                    "STOP-first race was not eligible for exact recovery");
        }
        require(!ores_native_actor_scheduler_claim_terminal_receipt(
                    scheduler, parent, child, &claimed),
                "raced terminal receipt was consumed twice");
        shutdown(scheduler);
    }
    // Recovery also binds the NEW physical parent's publication to the
    // one-use claim under the scheduler mutex, not a prior readiness snapshot.
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        auto* scheduler = ores_native_actor_scheduler_open_identity_control(
            2, 2, 2, data, nullptr, panic, nullptr);
        require(scheduler != nullptr, "recovered-parent race scheduler unavailable");
        const auto parent = ores_native_actor_scheduler_spawn(scheduler, 0);
        const auto child = ores_native_actor_scheduler_spawn(scheduler, 1);
        expect_done(scheduler, child);
        expect_done(scheduler, parent);
        const OresNativeActorTerminalFailure failure{
            ORES_NATIVE_CONTROL_PANIC, child, 915, 670
        };
        require(!ores_native_actor_scheduler_publish_failure(
                    scheduler, parent, failure),
                "stopped original parent accepted terminal failure");
        require(ores_native_actor_scheduler_seal_normal_exit(scheduler, parent),
                "original stopped parent could not seal normal");
        const auto recovered =
            ores_native_actor_scheduler_recycle_normal_done(scheduler, parent);
        require(recovered.generation == parent.generation + 1,
                "replacement parent generation absent");
        OresNativeRestartLease lease{};
        require(ores_native_actor_scheduler_acquire_restart_lease(
                    scheduler, recovered, &lease)
                  && ores_native_actor_scheduler_publish_restart_lease(
                    scheduler, lease)
                  && ores_native_actor_scheduler_release_restart_lease(
                    scheduler, lease), "replacement parent not published");
        OresNativeTerminalReceipt forged{};
        require(!ores_native_actor_scheduler_claim_recovered_parent_terminal_receipt(
                    scheduler, parent, OresNativeActorRef{
                        recovered.scheduler_domain, recovered.actor_slot,
                        recovered.generation + 1}, child, &forged),
                "forged replacement-parent generation claimed a PANIC");
        std::atomic<bool> start{false}, won{false};
        OresNativeTerminalReceipt claimed{};
        std::thread claimant([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            won.store(ores_native_actor_scheduler_claim_recovered_parent_terminal_receipt(
                scheduler, parent, recovered, child, &claimed) != 0);
        });
        std::thread stopper([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void) ores_native_actor_scheduler_stop(scheduler, recovered);
        });
        start.store(true, std::memory_order_release);
        claimant.join();
        stopper.join();
        require(ores_native_actor_scheduler_wait_done(scheduler, recovered),
                "replacement parent STOP did not physically finish");
        OresNativeTerminalReceipt after{};
        require(ores_native_actor_scheduler_terminal_receipt(
                    scheduler, parent, child, &after),
                "recovered-parent STOP erased terminal evidence");
        if (won.load()) {
            require(after.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                    && claimed.nonce != 0,
                    "pre-STOP recovered claim lost its exact token");
        } else {
            require(after.status == ORES_NATIVE_TERMINAL_RECEIPT_RECORDED,
                    "STOP-first recovered claim prematurely consumed evidence");
            require(!ores_native_actor_scheduler_claim_recovered_parent_terminal_receipt(
                        scheduler, parent, recovered, child, &claimed),
                    "post-STOP recovered callback bypassed publication fence");
            require(ores_native_actor_scheduler_claim_terminal_receipt(
                        scheduler, parent, child, &claimed),
                    "STOP-first claim lost quarantined recovery evidence");
        }
        require(!ores_native_actor_scheduler_claim_terminal_receipt(
                    scheduler, parent, child, &forged),
                "terminal evidence admitted two recovery claimants");
        shutdown(scheduler);
    }
    std::puts("NATIVE_RECOVERED_PARENT_STOP_RACE_PASS iterations=50");

    std::puts("NATIVE_LIVE_PARENT_CLAIM_STOP_RACE_PASS iterations=100");

    std::puts("NATIVE_TERMINAL_RECEIPT_RECONCILE_PASS"
              " lost_parent=1 recovered=1 exact_once=1 uncertain=1 stale=1"
              " claimant_races=20");
}
