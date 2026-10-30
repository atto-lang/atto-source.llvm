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
}
