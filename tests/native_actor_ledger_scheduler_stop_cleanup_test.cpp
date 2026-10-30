// Full NativeActorScheduler + ActorLedgerLeaseAdapter test: requires real scheduler.
// Tests an externally issued STOP while original actor DATA frame is retained.
#include "oreslang/runtime/ActorLedgerLeaseAdapter.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using Adapter = oreslang::runtime::ActorLedgerLeaseAdapter<4>;
using S = Adapter::Status;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "ACTOR_LEDGER_STOP_CLEANUP_FAIL %s:%d %s\n", \
        __FILE__, __LINE__, #x); std::abort(); \
} } while (0)
struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    std::unique_ptr<Adapter> ledger;
    Adapter::Token first{};
    OresNativeRetainedDataTicket retained{};
    std::atomic<int> entered{0}, release_data{0};
    std::atomic<int> cleanup_entered{0}, release_cleanup{0};
    std::atomic<int> data_calls{0}, cleanup_calls{0};
};
static void wait_for(const std::atomic<int>& flag, const char* name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "ACTOR_LEDGER_STOP_TIMEOUT %s\n", name);
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
extern "C" void actor_data(void* raw, size_t index, std::uint64_t payload) {
    auto& f = *static_cast<Fixture*>(raw);
    CHECK(index == 0 && payload == 17);
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler, f.actor));
    CHECK(f.ledger->current_actor_lease());
    Adapter::Selection choice{};
    CHECK(f.ledger->admit(Adapter::Kind::Mail, f.first) == S::Ok);
    CHECK(f.ledger->claim(choice) == S::Ok);
    CHECK(choice.token.sequence == f.first.sequence);
    CHECK(f.ledger->prepare_await(f.first) == S::Ok);
    CHECK(f.ledger->suspend(f.first) == S::Ok);
    CHECK(ores_native_actor_scheduler_retain_current_data(f.scheduler, &f.retained));
    f.data_calls.fetch_add(1, std::memory_order_relaxed);
    f.entered.store(1, std::memory_order_release);
    wait_for(f.release_data, "release original data carrier");
    // The native callback returns but the logical guest frame is retained.
}
extern "C" int actor_cleanup(void* raw, size_t index,
                               OresNativeRetainedDataTicket ticket) {
    auto& f = *static_cast<Fixture*>(raw);
    CHECK(index == 0);
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler, f.actor));
    CHECK(ticket.actor.generation == f.actor.generation);
    CHECK(ticket.slot == f.retained.slot);
    CHECK(ticket.slot_generation == f.retained.slot_generation);
    CHECK(ticket.execution_id == f.retained.execution_id);
    CHECK(ores_native_actor_scheduler_retained_data_count(f.scheduler, f.actor) == 1);
    // A scheduler-owned cleanup turn, not the STOP caller, owns mutation.
    CHECK(f.ledger->current_actor_lease());
    CHECK(f.ledger->stop_and_request_cleanup() == S::Ok);
    Adapter::Selection selection{};
    CHECK(f.ledger->claim(selection) == S::Ok);
    CHECK(selection.token.sequence == f.first.sequence);
    CHECK(selection.dispatch == Adapter::Ledger::Dispatch::CancelCleanup);
    f.cleanup_calls.fetch_add(1, std::memory_order_relaxed);
    f.cleanup_entered.store(1, std::memory_order_release);
    wait_for(f.release_cleanup, "physical guest cleanup");
    CHECK(f.ledger->finish_after_guest_exit(f.first) == S::Ok);
    Adapter::Snapshot snapshot{};
    CHECK(f.ledger->snapshot(snapshot));
    CHECK(snapshot.closed && snapshot.live == 0 && !snapshot.executing);
    // Returning success authenticates physical exit before the scheduler
    // acknowledges abandoned DATA and permits terminal heap reclamation.
    return 1;
}
int main() {
    Fixture f;
    f.scheduler = ores_native_actor_scheduler_open(1, 3, 1, actor_data, &f);
    CHECK(f.scheduler);
    CHECK(ores_native_actor_scheduler_configure_retained_cleanup(
        f.scheduler, actor_cleanup));
    f.actor = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    CHECK(f.actor.scheduler_domain != 0);
    f.ledger = std::make_unique<Adapter>(f.scheduler, f.actor, 7001);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actor, 17));
    wait_for(f.entered, "actor first suspension");
    // B is accepted behind suspended A, then STOP seals and abandons B.
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actor, 18));
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler, f.actor));
    CHECK(f.ledger->stop_and_request_cleanup() == S::Unauthorized);
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler, f.actor));
    CHECK(ores_native_actor_scheduler_retained_data_count(f.scheduler, f.actor) == 1);
    f.release_data.store(1, std::memory_order_release);
    wait_for(f.cleanup_entered, "authenticated cleanup turn");
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler, f.actor));
    CHECK(f.cleanup_calls.load() == 1);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(f.scheduler, f.retained));
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(f.scheduler, f.retained));
    f.release_cleanup.store(1, std::memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.actor));
    CHECK(f.data_calls.load() == 1 && f.cleanup_calls.load() == 1);
    CHECK(!ores_native_actor_scheduler_retained_data_count(f.scheduler, f.actor));
    OresNativeActorTerminalState terminal{};
    CHECK(ores_native_actor_scheduler_terminal_state(f.scheduler, f.actor, &terminal));
    CHECK(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    CHECK(terminal.accepted_user_messages == 2);
    CHECK(terminal.processed_user_messages == 0);
    CHECK(terminal.abandoned_user_messages == 2);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    std::puts("ACTOR_LEDGER_NATIVE_STOP_CLEANUP_PASS");
}
