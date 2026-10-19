// Supervisor fault-injection case: STOP wins after immutable image promotion
// but before native mailbox route publication. Must fail TERMINALLY closed.
// Host-only integration proof, not compiler-emitted .ores/FFI or JIT code.
#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorHeapIdentity;
struct Code { int version; };
using Gate = oreslang::runtime::NativeActorHotUpgradeGate<Code>;
struct Host {
    Gate* gate;
    ActorHeapIdentity id;
    std::atomic<int> count{0};
    std::atomic<int> last{0};
};
static void check(bool result, const char* reason) {
    if (!result) throw std::runtime_error(reason);
}
static void deliver(void* raw, size_t slot, std::uint64_t payload) {
    Host* host = static_cast<Host*>(raw);
    if (slot != 0 || payload != 1) std::abort();
    try {
        auto pin = host->gate->pin(host->id);
        host->last.store(pin.code().version, std::memory_order_relaxed);
        host->count.fetch_add(1, std::memory_order_release);
    } catch (...) { std::abort(); }
}
static void wait_count(Host& host, int expected) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(5);
    while (host.count.load(std::memory_order_acquire) != expected
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(host.count.load(std::memory_order_acquire) == expected,
          "native actor callback did not complete");
}
static void wait_empty(OresNativeActorScheduler* s,
                       OresNativeActorRef actor, std::uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(5);
    while (!ores_native_actor_scheduler_upgrade_quiesced(s, actor, ticket)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(ores_native_actor_scheduler_upgrade_quiesced(s, actor, ticket),
          "scheduler did not finish an acknowledged turn");
}
int main() {
    try {
        std::atomic<unsigned> reclaimed{0};
        ActorHeapIdentity id{55, 77, 1};
        Gate::Shape shape{
            Gate::Kind::trusted_shared, "Counter<int>",
            "value:int", "none", "trusted"
        };
        Gate gate(id, shape, Code{1}, [&]{ ++reclaimed; });
        Host host{&gate, id};
        OresNativeActorScheduler* scheduler =
            ores_native_actor_scheduler_open(1, 4, 1, deliver, &host);
        check(scheduler != nullptr, "scheduler open failed");
        OresNativeActorRef actor =
            ores_native_actor_scheduler_spawn(scheduler, 0);
        check(actor.scheduler_domain != 0, "actor spawn failed");
        check(ores_native_actor_scheduler_try_send(scheduler, actor, 1)
              == ORES_NATIVE_SEND_ACCEPTED, "v1 message rejected");
        wait_count(host, 1);
        check(host.last.load() == 1, "old code not executed");

        std::uint64_t admission_ticket = 0;
        check(ores_native_actor_scheduler_upgrade_freeze(
            scheduler, actor, 1, &admission_ticket),
            "scheduler freeze failed");
        wait_empty(scheduler, actor, admission_ticket);
        auto ticket = gate.freeze(id, 1);
        check(gate.quiesced(ticket), "old image still pinned");
        const auto published = gate.promote(ticket, shape, Code{2},
            [&]{ ++reclaimed; });
        check(published.revision == 2, "new image not activated");
        check(reclaimed == 1, "old code not retired");

        // Inject a terminal STOP at the exact dangerous gap: code v2 has
        // published, scheduler route epoch remains 1 and DATA stays sealed.
        check(ores_native_actor_scheduler_stop(scheduler, actor),
              "stop must win over pending upgrade");
        check(!ores_native_actor_scheduler_upgrade_finish(
                  scheduler, actor, admission_ticket, 1),
              "stopped actor accepted route publication");
        check(!ores_native_actor_scheduler_upgrade_finish(
                  scheduler, actor, admission_ticket, 0),
              "stopped actor rolled back and reopened DATA");
        check(ores_native_actor_scheduler_try_send(scheduler, actor, 1)
              == ORES_NATIVE_SEND_TERMINATED,
              "stopped actor accepted new message");
        check(ores_native_actor_scheduler_wait_done(scheduler, actor),
              "stopped actor failed its done barrier");
        check(host.count.load() == 1,
              "unexpected duplicate actor execution across upgrade failure");
        gate.close();
        check(reclaimed == 2, "new code not reclaimed on terminal failure");
        ores_native_actor_scheduler_retire(scheduler);
        check(ores_native_actor_scheduler_drain(scheduler),
              "mailbox did not drain on terminal failure");
        check(ores_native_actor_scheduler_try_destroy(scheduler),
              "scheduler not safely reclaimed");
        std::cout << "NATIVE_ACTOR_UPGRADE_PARTIAL_PUBLISH_STOP_PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "partial publication regression: "
                  << ex.what() << '\n';
        return 1;
    }
}
