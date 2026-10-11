#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSupervisorPanicBridge.hpp"
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
    std::shared_ptr<SupervisorProcess> process;
    std::unique_ptr<NativeSupervisorPanicBridge> bridge;
    std::atomic<unsigned> terminal_events{0};
    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> applied{0};
    std::atomic<unsigned> failed_checks{0};
    std::atomic<std::uint64_t> observed_child{0};
    std::atomic<std::uint64_t> observed_incarnation{0};
};

static void ordinary(void*, size_t, std::uint64_t) {}

static void typed_terminal(void* raw, size_t parent_slot,
                           OresNativeActorTerminalFailure event) {
    auto& f = *static_cast<Fixture*>(raw);
    ++f.terminal_events;
    if (f.bridge && f.bridge->on_panic(parent_slot, event))
        ++f.accepted;
    else
        ++f.failed_checks;
}

static void exercise(bool ambiguous) {
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
                require(ores_native_actor_scheduler_poll_done(f.scheduler, f.child),
                        "policy ran before child physical finalization");
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
            if (ambiguous)
                throw std::runtime_error("unknown external STOP commit");
        });
    f.bridge = std::make_unique<NativeSupervisorPanicBridge>(
        f.scheduler, f.parent, f.process, f.registry);
    require(f.bridge->trusted_child_registry() == f.registry,
            "panic bridge did not share the source installer's identity registry");
    f.bridge->register_child(f.child);
    require(f.registry->tracked_children() == 1,
            "physical child spawn was not registered with supervisor");

    require(ores_native_actor_scheduler_stop(f.scheduler, f.child),
            "physical child STOP request rejected");
    require(ores_native_actor_scheduler_publish_failure(
                f.scheduler, f.parent,
                OresNativeActorTerminalFailure{
                    ORES_NATIVE_CONTROL_PANIC, f.child, 17, 91}),
            "typed terminal event not admitted to native scheduler");

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f.applied.load() == 1
            && f.accepted.load() == 1 && f.terminal_events.load() == 1
            && (ambiguous ? f.process->undelivered() == 1
                          : f.process->resolved() == 1))
            break;
        std::this_thread::sleep_for(1ms);
    }
    require(f.terminal_events == 1 && f.accepted == 1,
            "typed native panic did not pass the bridge exactly once");
    require(f.applied == 1 && f.failed_checks == 0,
            "source STOP effect missing or failed runtime invariants");
    require(f.observed_child != 0 && f.observed_incarnation == 1,
            "host did not receive trusted first logical child generation");
    const auto logical = SupervisorChildRegistry::ChildRef{
        supervisor, domain, f.observed_child.load()};
    const auto state = f.registry->snapshot(logical);
    require(state.phase == (ambiguous
                ? SupervisorChildRegistry::Phase::stopping
                : SupervisorChildRegistry::Phase::stopped),
            "external STOP result did not produce correct terminal disposition");
    const auto receipt = f.registry->stop_receipt(logical);
    require(receipt.status == (ambiguous
                ? SupervisorChildRegistry::StopReceiptStatus::uncertain
                : SupervisorChildRegistry::StopReceiptStatus::acknowledged),
            "host STOP receipt did not preserve applied versus ambiguous outcome");
    require(receipt.turn_generation != 0
                && receipt.failed_actor.actor_slot == f.child.actor_slot
                && receipt.incarnation == 1,
            "STOP receipt was not bound to the exact physical incarnation");
    require(f.process->resolved() == (ambiguous ? 0U : 1U)
                && f.process->undelivered() == (ambiguous ? 1U : 0U),
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
    std::puts("NATIVE_SOURCE_SUPERVISOR_BRIDGE_E2E_PASS typed=1 stop=1 unknown_quarantined=1");
}
