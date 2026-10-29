#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <stdexcept>

using namespace oreslang::runtime;

template <typename T, typename F>
static void rejects(F&& f) {
    bool failed = false;
    try { f(); } catch (const T&) { failed = true; }
    assert(failed);
}

int main() {
    OresVM vm;
    IsolateConfig cfg;
    cfg.tenant_id = "tenant-A";
    cfg.heap_limit_bytes = 1024;
    cfg.max_actors = 2;
    cfg.durable_queues = {2, 4, 3, 16};
    auto a = vm.create_isolate(cfg);
    vm.start(a);
    cfg.tenant_id = "tenant-B";
    auto b = vm.create_isolate(cfg);
    vm.start(b);

    auto a1 = vm.enqueue_durable(a, 7, 1, 4);
    auto a2 = vm.enqueue_durable(a, 7, 2, 4);
    auto a3 = vm.enqueue_durable(a, 7, 3, 4);
    auto a4 = vm.enqueue_durable(a, 9, 4, 4);
    auto b1 = vm.enqueue_durable(b, 7, 1, 4);
    assert(a1.runnable && !a2.runnable && !a3.runnable && a4.runnable);
    assert(b1.runnable);
    rejects<std::logic_error>([&] { vm.release_durable(a, b1.ticket); });
    rejects<std::logic_error>([&] { vm.release_durable(a, a2.ticket); });
    rejects<std::length_error>([&] { vm.enqueue_durable(a, 10, 5, 1); });
    rejects<std::logic_error>([&] { vm.enqueue_durable(a, 7, 3, 1); });

    auto second = vm.release_durable(a, a1.ticket);
    assert(second && *second == a2.ticket);
    vm.cancel_durable_waiter(a, a3.ticket);
    rejects<std::logic_error>([&] { vm.release_durable(a, a1.ticket); });
    assert(!vm.release_durable(a, a2.ticket));
    assert(!vm.release_durable(a, a4.ticket));
    assert(!vm.release_durable(b, b1.ticket));
    auto resumed = vm.enqueue_durable(a, 7, 1, 1);
    assert(resumed.ticket.sequence != a1.ticket.sequence);

    vm.begin_shutdown(a);
    rejects<std::logic_error>([&] { vm.enqueue_durable(a, 7, 90, 1); });
    rejects<std::logic_error>([&] { vm.complete_shutdown(a); });
    assert(!vm.release_durable(a, resumed.ticket));
    assert(vm.durable_snapshot(a).entries == 0);
    vm.complete_shutdown(a);
    vm.destroy(a);
    rejects<std::logic_error>([&] { vm.enqueue_durable(a, 7, 8, 1); });

    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);
    return 0;
}
