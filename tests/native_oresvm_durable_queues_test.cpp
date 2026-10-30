#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
using namespace oreslang::runtime;

template <typename E, typename F> static void rejects(F&& f) {
    bool failed = false;
    try { f(); } catch (const E&) { failed = true; }
    assert(failed);
}


// Native guest boundary race fixture. A host-issued lock must not be released
// while this synchronous guest is still physically executing.
static std::atomic<bool> dispatch_entered{false};
static std::atomic<bool> dispatch_release{false};
extern "C" std::int32_t blocking_durable_guest(
        OresIsolateContext*, std::int64_t delta, std::int64_t* result) {
    dispatch_entered.store(true, std::memory_order_release);
    while (!dispatch_release.load(std::memory_order_acquire))
        std::this_thread::yield();
    *result = delta;
    return ORES_GUEST_OK;
}

extern "C" std::int32_t throwing_durable_guest(
        OresIsolateContext* context, std::int64_t, std::int64_t*) {
    // An exception after modifying isolate-owned globals must quarantine
    // the tenant and forbid further durable claims/admissions.
    const auto written = context->store_i64(context->call_data, 0, 19);
    if (!written) throw std::runtime_error("guest store failed");
    throw std::runtime_error("guest exception after partial state write");
}

extern "C" std::int32_t actor_scalar_guest(
        OresIsolateContext* ctx, std::int64_t delta, std::int64_t* output) {
    if (!ctx || !output || ctx->abi_version != ORES_ISOLATE_ABI_VERSION ||
        ctx->slot_count != ORES_ISOLATE_I64_SLOTS) return ORES_GUEST_BAD_CONTEXT;
    std::int64_t old = 0;
    if (ctx->load_i64(ctx->call_data, 0, &old) != 1) return ORES_GUEST_LOAD_DENIED;
    if ((delta > 0 && old > INT64_MAX - delta) ||
        (delta < 0 && old < INT64_MIN - delta)) return ORES_GUEST_OVERFLOW;
    const auto next = old + delta;
    if (ctx->store_i64(ctx->call_data, 0, next) != 1) return ORES_GUEST_STORE_DENIED;
    *output = next;
    return ORES_GUEST_OK;
}

// Two independent actor turns must overlap in physical native execution.
// The host never holds its root registry mutex across user-code callbacks.
static std::atomic<unsigned> parallel_entered{0};
static std::atomic<bool> parallel_release{false};
extern "C" std::int32_t parallel_actor_guest(
        OresIsolateContext*, std::int64_t value, std::int64_t* out) {
    parallel_entered.fetch_add(1, std::memory_order_release);
    while (!parallel_release.load(std::memory_order_acquire))
        std::this_thread::yield();
    *out = value;
    return ORES_GUEST_OK;
}

int main() {
    OresVM vm;
    IsolateConfig cfg;
    cfg.tenant_id = "tenant-A";
    cfg.heap_limit_bytes = 1024;
    cfg.max_actors = 3;
    cfg.durable_queues = {2, 4, 3, 16};
    auto a = vm.create_isolate(cfg);
    vm.start(a);
    auto actor_a1 = vm.register_actor(a, 1);
    auto actor_a2 = vm.register_actor(a, 2);
    cfg.tenant_id = "tenant-B";
    auto b = vm.create_isolate(cfg);
    vm.start(b);
    auto actor_b1 = vm.register_actor(b, 1);

    auto a1 = vm.enqueue_durable(a, actor_a1, 7, 1, 4);
    auto a2 = vm.enqueue_durable(a, actor_a1, 7, 2, 4);
    auto a3 = vm.enqueue_durable(a, actor_a1, 9, 3, 4);
    auto a4 = vm.enqueue_durable(a, actor_a2, 9, 4, 4);
    auto b1 = vm.enqueue_durable(b, actor_b1, 7, 1, 4);
    assert(a1.lock_head && !a2.lock_head && a3.lock_head && !a4.lock_head);
    assert(b1.lock_head);
    rejects<std::logic_error>([&] { vm.claim_durable(a, b1.ticket); });
    rejects<std::logic_error>([&] { vm.finish_durable(a, a1.ticket); });
    rejects<std::length_error>([&] { vm.enqueue_durable(a, actor_a1, 10, 5, 1); });
    rejects<std::logic_error>([&] { vm.enqueue_durable(a, actor_a1, 7, 3, 1); });
    rejects<std::logic_error>([&] { vm.retire_actor(a, actor_a1); });

    assert(vm.claim_durable(a, a1.ticket));
    assert(!vm.claim_durable(a, a3.ticket)); // same actor; different locks
    rejects<std::logic_error>([&] { vm.cancel_durable_unstarted_head(a, a1.ticket); });
    assert(vm.finish_durable(a, a1.ticket) == a2.ticket);
    assert(vm.claim_durable(a, a3.ticket));
    assert(!vm.claim_durable(a, a2.ticket));
    assert(vm.finish_durable(a, a3.ticket) == a4.ticket);
    assert(vm.claim_durable(a, a2.ticket));
    assert(vm.claim_durable(a, a4.ticket)); // different actors
    assert(!vm.finish_durable(a, a4.ticket));
    assert(!vm.finish_durable(a, a2.ticket));
    auto unstarted = vm.enqueue_durable(a, actor_a1, 7, 5, 1);
    assert(!vm.cancel_durable_unstarted_head(a, unstarted.ticket));
    auto fresh = vm.enqueue_durable(a, actor_a1, 7, 1, 1);
    assert(fresh.ticket.sequence != a1.ticket.sequence);
    vm.begin_shutdown(a);
    rejects<std::logic_error>([&] { vm.claim_durable(a, fresh.ticket); });
    rejects<std::logic_error>([&] { vm.complete_shutdown(a); });
    assert(!vm.cancel_durable_unstarted_head(a, fresh.ticket));
    vm.retire_actor(a, actor_a1);
    vm.retire_actor(a, actor_a2);
    vm.complete_shutdown(a);
    vm.destroy(a);
    rejects<std::logic_error>([&] { vm.enqueue_durable(a, actor_a1, 7, 9, 1); });

    assert(vm.claim_durable(b, b1.ticket));
    assert(!vm.finish_durable(b, b1.ticket));
    vm.retire_actor(b, actor_b1);
    rejects<std::logic_error>([&] { vm.enqueue_durable(b, actor_b1, 7, 22, 1); });
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);

    // Two roots both start at isolate-generation 1: never accept each
    // other's matching numeric ticket, including cancel and claim.
    OresVM first_root, second_root;
    auto first_iso = first_root.create_isolate({"tenant", 1024, 2});
    auto second_iso = second_root.create_isolate({"tenant", 1024, 2});
    first_root.start(first_iso); second_root.start(second_iso);
    auto first_actor=first_root.register_actor(first_iso, 1);
    auto second_actor=second_root.register_actor(second_iso, 1);
    auto first_ticket=first_root.enqueue_durable(first_iso, first_actor, 7, 1, 1);
    auto second_ticket=second_root.enqueue_durable(second_iso, second_actor, 7, 1, 1);
    rejects<std::logic_error>([&] { first_root.claim_durable(first_iso, second_ticket.ticket); });
    rejects<std::logic_error>([&] { first_root.cancel_durable_unstarted_head(first_iso, second_ticket.ticket); });
    first_root.cancel_durable_unstarted_head(first_iso, first_ticket.ticket);
    second_root.cancel_durable_unstarted_head(second_iso, second_ticket.ticket);
    first_root.retire_actor(first_iso, first_actor);
    second_root.retire_actor(second_iso, second_actor);
    first_root.begin_shutdown(first_iso); first_root.complete_shutdown(first_iso); first_root.destroy(first_iso);
    second_root.begin_shutdown(second_iso); second_root.complete_shutdown(second_iso); second_root.destroy(second_iso);

    OresVM threaded;
    auto t_iso=threaded.create_isolate({"threaded",1024,2});
    threaded.start(t_iso);
    auto t_actor=threaded.register_actor(t_iso,1);
    auto t_work=threaded.enqueue_durable(t_iso,t_actor,5,1,1);
    std::atomic<bool> completed{false};
    std::thread worker([&] {
        auto receipt=threaded.try_invoke_durable_guest_result(
            t_iso,t_work.ticket,blocking_durable_guest,42);
        assert(receipt && receipt->outcome.status==ORES_GUEST_OK);
        assert(receipt->outcome.value==42);
        completed.store(true,std::memory_order_release);
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!dispatch_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now()<deadline)
        std::this_thread::yield();
    // Ensure the worker cannot remain blocked if assertion throws/aborts:
    // release before the final join even if the admission check fails.
    const bool did_enter=dispatch_entered.load(std::memory_order_acquire);
    if (did_enter) {
        rejects<std::logic_error>([&] { threaded.finish_durable(t_iso,t_work.ticket); });
        rejects<std::logic_error>([&] { threaded.retire_actor(t_iso,t_actor); });
        threaded.begin_shutdown(t_iso);
        rejects<std::logic_error>([&] { threaded.complete_shutdown(t_iso); });
    }
    dispatch_release.store(true,std::memory_order_release);
    worker.join();
    assert(did_enter && completed.load(std::memory_order_acquire));
    assert(threaded.durable_snapshot(t_iso).entries==0);
    threaded.retire_actor(t_iso,t_actor);
    threaded.complete_shutdown(t_iso);
    threaded.destroy(t_iso);

    // Host-only actor state parity: two actors in one tenant must never
    // share source-generated slot-zero state, even across different lock IDs.
    OresVM scalar_vm;
    auto scalar_tenant = scalar_vm.create_isolate({"actor-scalars", 1024, 3});
    scalar_vm.start(scalar_tenant);
    auto first = scalar_vm.register_actor(scalar_tenant, 1);
    auto second = scalar_vm.register_actor(scalar_tenant, 2);
    auto turn1 = scalar_vm.enqueue_durable(scalar_tenant, first, 7, 1, 1);
    auto turn2 = scalar_vm.enqueue_durable(scalar_tenant, second, 19, 2, 1);
    auto first_result = scalar_vm.try_invoke_durable_guest_result(
        scalar_tenant, turn1.ticket, actor_scalar_guest, 9);
    auto second_result = scalar_vm.try_invoke_durable_guest_result(
        scalar_tenant, turn2.ticket, actor_scalar_guest, 2);
    assert(first_result && first_result->outcome.value == 9);
    assert(second_result && second_result->outcome.value == 2);
    auto read_first = scalar_vm.enqueue_durable(scalar_tenant, first, 7, 3, 1);
    auto read_second = scalar_vm.enqueue_durable(scalar_tenant, second, 19, 4, 1);
    assert(scalar_vm.try_invoke_durable_guest_result(scalar_tenant,
        read_first.ticket, actor_scalar_guest, 0)->outcome.value == 9);
    assert(scalar_vm.try_invoke_durable_guest_result(scalar_tenant,
        read_second.ticket, actor_scalar_guest, 0)->outcome.value == 2);
    assert(scalar_vm.invoke_guest(scalar_tenant, actor_scalar_guest, 0) == 0);
    scalar_vm.retire_actor(scalar_tenant, second);
    auto second_again = scalar_vm.register_actor(scalar_tenant, 2);
    auto new_turn = scalar_vm.enqueue_durable(scalar_tenant, second_again, 19, 5, 1);
    assert(scalar_vm.try_invoke_durable_guest_result(scalar_tenant,
        new_turn.ticket, actor_scalar_guest, 0)->outcome.value == 0);
    scalar_vm.retire_actor(scalar_tenant, second_again);
    scalar_vm.retire_actor(scalar_tenant, first);
    scalar_vm.begin_shutdown(scalar_tenant);
    scalar_vm.complete_shutdown(scalar_tenant);
    scalar_vm.destroy(scalar_tenant);

    // Two different actors within ONE tenant can be in guest code at the
    // same time; the tenant-global legacy scalar mutex no longer blocks them.
    OresVM parallel_vm;
    auto p_iso = parallel_vm.create_isolate({"parallel", 1024, 2});
    parallel_vm.start(p_iso);
    auto p_a1 = parallel_vm.register_actor(p_iso, 1);
    auto p_a2 = parallel_vm.register_actor(p_iso, 2);
    auto p_t1 = parallel_vm.enqueue_durable(p_iso, p_a1, 7, 1, 1);
    auto p_t2 = parallel_vm.enqueue_durable(p_iso, p_a2, 19, 2, 1);
    std::optional<OresVM::DurableGuestReceipt> p_result1;
    std::optional<OresVM::DurableGuestReceipt> p_result2;
    std::thread p_worker1([&] {
        p_result1 = parallel_vm.try_invoke_durable_guest_result(
            p_iso, p_t1.ticket, parallel_actor_guest, 11);
    });
    std::thread p_worker2([&] {
        p_result2 = parallel_vm.try_invoke_durable_guest_result(
            p_iso, p_t2.ticket, parallel_actor_guest, 22);
    });
    const auto p_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (parallel_entered.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < p_deadline)
        std::this_thread::yield();
    const bool both_started = parallel_entered.load(std::memory_order_acquire) == 2;
    parallel_release.store(true, std::memory_order_release);
    p_worker1.join();
    p_worker2.join();
    assert(both_started && p_result1 && p_result2);
    assert(p_result1->outcome.value == 11 && p_result2->outcome.value == 22);
    assert(parallel_vm.durable_snapshot(p_iso).entries == 0);
    parallel_vm.retire_actor(p_iso, p_a1);
    parallel_vm.retire_actor(p_iso, p_a2);
    parallel_vm.begin_shutdown(p_iso);
    parallel_vm.complete_shutdown(p_iso);
    parallel_vm.destroy(p_iso);

    OresVM quarantined;
    auto q_iso=quarantined.create_isolate({"quarantined",1024,2});
    quarantined.start(q_iso);
    auto q_actor=quarantined.register_actor(q_iso,1);
    auto q_work=quarantined.enqueue_durable(q_iso,q_actor,6,1,1);
    rejects<std::runtime_error>([&] {
        (void)quarantined.try_invoke_durable_guest_result(
            q_iso,q_work.ticket,throwing_durable_guest,0);
    });
    assert(quarantined.snapshot(q_iso).phase==IsolatePhase::Quiescing);
    assert(quarantined.durable_snapshot(q_iso).entries==0);
    rejects<std::logic_error>([&] {
        (void)quarantined.enqueue_durable(q_iso,q_actor,6,2,1);
    });
    quarantined.retire_actor(q_iso,q_actor);
    quarantined.complete_shutdown(q_iso);
    quarantined.destroy(q_iso);
}
