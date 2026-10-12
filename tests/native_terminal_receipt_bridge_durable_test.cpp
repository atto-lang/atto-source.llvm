#include "oreslang/runtime/NativeSupervisorPanicBridge.hpp"
#include "oreslang/runtime/SupervisorControlScheduler.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace oreslang::runtime;
using namespace std::chrono_literals;
static void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
struct Host {
    std::atomic<NativeSupervisorPanicBridge*> bridge{nullptr};
    std::atomic<bool> callback_done{false};
    std::atomic<bool> accepted{false};
};
static void task(void*, OresNativeActorRef, std::uint64_t) {}
static void panic(void* data, OresNativeActorRef parent,
                  OresNativeActorTerminalFailure failure) {
    auto& host = *static_cast<Host*>(data);
    const auto* bridge = host.bridge.load(std::memory_order_acquire);
    if (!bridge) return;
    host.accepted.store(
        const_cast<NativeSupervisorPanicBridge*>(bridge)->on_panic(
            parent, failure), std::memory_order_release);
    host.callback_done.store(true, std::memory_order_release);
}
static void stop_and_wait(OresNativeActorScheduler* s, OresNativeActorRef r) {
    check(ores_native_actor_scheduler_stop(s, r)
              && ores_native_actor_scheduler_wait_done(s, r),
          "actor did not physically STOP and reach DONE");
}
static void await_effect(const std::atomic<unsigned>& effects) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (effects.load(std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(effects.load() == 1, "real supervisor policy effect never committed");
}
static void shutdown(OresNativeActorScheduler* s) {
    ores_native_actor_scheduler_retire(s);
    check(ores_native_actor_scheduler_drain(s)
              && ores_native_actor_scheduler_try_destroy(s),
          "native supervisor scheduler teardown failed");
}
int main() {
    char dirbuf[] = "/tmp/ores-durable-bridge-XXXXXX";
    const char* dir = ::mkdtemp(dirbuf);
    check(dir != nullptr, "tmp dir creation failed");
    for (int mode = 0; mode < 2; ++mode) {
        const std::string path = std::string(dir) + "/receipt-" + std::to_string(mode);
        Host host{};
        auto* s = ores_native_actor_scheduler_open_identity_control(
            2, 2, 2, task, nullptr, panic, &host);
        check(s != nullptr, "native identity-mode scheduler unavailable");
        const auto parent = ores_native_actor_scheduler_spawn(s, 0);
        const auto child = ores_native_actor_scheduler_spawn(s, 1);
        check(parent.scheduler_domain != 0 && child.scheduler_domain != 0,
              "parent/child physical identities unavailable");
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        std::atomic<unsigned> calls{0}, effects{0};
        auto process = std::make_shared<SupervisorProcess>(
            7801 + mode, parent.scheduler_domain, 4,
            control->adapter(),
            [&](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                ++calls;
                return SupervisorProcess::Directive::acknowledge;
            },
            [&](const SupervisorProcess::Resolution&) { ++effects; });
        auto registry = std::make_shared<SupervisorChildRegistry>(
            process->supervisor_id(), parent.scheduler_domain);
        auto journal = std::make_shared<NativeTerminalReceiptJournal>(path);
        auto bridge = std::make_unique<NativeSupervisorPanicBridge>(
            s, parent, process, registry, journal);
        host.bridge.store(bridge.get(), std::memory_order_release);
        bridge->register_child(child);
        const auto transitions = registry->committed_transitions();
        check(!transitions.empty() && transitions.front().state,
              "logical child registration omitted durable identity");
        const auto logical = transitions.front().state->child.logical_child_id;
        stop_and_wait(s, child);
        const OresNativeActorTerminalFailure failure{
            ORES_NATIVE_CONTROL_PANIC, child, 551, 882
        };
        auto key = DurableTerminalKey::from(
            process->supervisor_id(), process->runtime_id(),
            parent.scheduler_domain, parent.actor_slot, parent.generation,
            child.scheduler_domain, child.actor_slot, child.generation,
            failure.kind, failure.failure_code, failure.correlation,
            logical, 1);
        auto journal_before = journal->find(key);
        check(!journal_before, "terminal journal acquired event before admission");
        if (mode == 0) {
            // A direct call from the same host thread must not mint a disk
            // receipt; only the actual callback carrier can preflight.
            check(!bridge->on_panic(parent, failure)
                      && !journal->find(key),
                  "off-carrier host call poisoned durable receipt");
            check(ores_native_actor_scheduler_publish_failure(
                      s, parent, failure),
                  "physical PANIC was not published to CONTROL");
            auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!host.callback_done.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            check(host.callback_done.load(std::memory_order_acquire)
                      && host.accepted.load(std::memory_order_acquire),
                  "durable CONTROL callback was not accepted");
        } else {
            // The original parent STOPs before CONTROL delivery. A later
            // generation must claim the SAME fsynced original-child receipt.
            stop_and_wait(s, parent);
            check(!ores_native_actor_scheduler_publish_failure(
                      s, parent, failure),
                  "stopped original parent unexpectedly accepted CONTROL");
            check(ores_native_actor_scheduler_seal_normal_exit(s, parent),
                  "original parent normal-exit was not sealed");
            const auto recovered = ores_native_actor_scheduler_recycle_normal_done(
                s, parent);
            check(recovered.scheduler_domain == parent.scheduler_domain
                      && recovered.generation == parent.generation + 1,
                  "recovered parent did not advance incarnation");
            OresNativeRestartLease lease{};
            check(ores_native_actor_scheduler_acquire_restart_lease(
                      s, recovered, &lease)
                      && ores_native_actor_scheduler_publish_restart_lease(s, lease)
                      && ores_native_actor_scheduler_release_restart_lease(s, lease),
                  "recovered parent was not physically published");
            check(bridge->reconcile_quarantined_terminal(recovered, child),
                  "durable recovered-parent PANIC admission failed");
            stop_and_wait(s, recovered);
        }
        await_effect(effects);
        auto recorded = journal->inspect(key);
        check(recorded.phase == DurableTerminalPhase::claimed
                  && recorded.nonce > 0 && recorded.ambiguous()
                  && calls.load() == 1 && effects.load() == 1,
              "real CONTROL effect was not durably quarantined");
        auto evidence = ExternalEffectProof{key, recorded.nonce, {}};
        for (unsigned i = 0; i < evidence.evidence_sha256.size(); ++i)
            evidence.evidence_sha256[i] = static_cast<std::uint8_t>(i + 1);
        bool legacy_bypass = false;
        try {
            (void)bridge->reconcile_claimed_terminal_ack(
                parent, child, [](const auto&) { return true; });
        } catch (const std::logic_error&) { legacy_bypass = true; }
        check(legacy_bypass, "legacy scheduler ACK bypassed durable journal proof");
        auto foreign = evidence;
        foreign.key.words[4]++;
        bool foreign_denied = false;
        try {
            (void)bridge->reconcile_durable_external_effect(
                foreign, [](const auto&) { return true; });
        } catch (const std::logic_error&) { foreign_denied = true; }
        check(foreign_denied, "foreign original-parent proof bypassed journal owner");
        check(!bridge->reconcile_durable_external_effect(
                  evidence, [](const auto&) { return false; }),
              "unverified CONTROL effect released durable quarantine");
        check(bridge->reconcile_durable_external_effect(
                  evidence, [&](const auto& p) {
                      return calls.load() == 1 && effects.load() == 1
                          && p.key == key && p.nonce == recorded.nonce;
                  }), "independently checked host CONTROL effect not proven");
        check(journal->inspect(key).phase == DurableTerminalPhase::acknowledged,
              "durable effect ACK did not persist");
        if (mode == 0) stop_and_wait(s, parent);
        bridge.reset();
        host.bridge.store(nullptr);
        process->close();
        control->close();
        shutdown(s);
        journal.reset();
        {
            NativeTerminalReceiptJournal reopened(path);
            check(reopened.inspect(key).phase == DurableTerminalPhase::acknowledged,
                  "recovered host commit acknowledgement did not survive reopen");
            bool duplicate = false;
            try { (void)reopened.claim(key); }
            catch (const std::logic_error&) { duplicate = true; }
            check(duplicate && effects.load() == 1,
                  "restarted host duplicated committed external effect");
        }
        check(::unlink(path.c_str()) == 0, "journal unlink failed");
    }
    check(::rmdir(dir) == 0, "journal test directory cleanup failed");
    std::puts("NATIVE_DURABLE_SUPERVISOR_BRIDGE_PASS"
              " original=1 recovered=1 off_carrier=0 claim_once=1"
              " external_ack_proof=1 no_replay=1 legacy_bypass=0");
}
