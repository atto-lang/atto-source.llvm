#include "oreslang/runtime/OresVM.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

extern "C" std::int32_t ores_guest_entry(OresIsolateContext*, std::int64_t, std::int64_t*);

using oreslang::runtime::OresVM;
using oreslang::runtime::DurableLockQueues;
int main() {
    OresVM vm;
    auto a = vm.create_isolate({"tenant-a", 4096, 3});
    auto b = vm.create_isolate({"tenant-b", 4096, 3});
    vm.start(a); vm.start(b);
    auto a_actor = vm.register_actor(a, 1);
    auto b_actor = vm.register_actor(b, 1);
    // The SAME source-compiled .ores scalar step and numeric lock ID are
    // executed against two disjoint tenant-owned isolate globals.
    for (int i = 0; i < 1000; ++i) {
        auto work = vm.enqueue_durable(a, a_actor, 7, static_cast<std::uint64_t>(i+1), 1);
        assert(work.lock_head);
        auto receipt = vm.try_invoke_durable_guest_result(a, work.ticket, ores_guest_entry, 1);
        assert(receipt && receipt->outcome.status == ORES_GUEST_OK);
        assert(receipt->outcome.value == i+1 && !receipt->next_head);
    }
    for (int i = 0; i < 500; ++i) {
        auto work = vm.enqueue_durable(b, b_actor, 7, static_cast<std::uint64_t>(i+1), 1);
        auto receipt = vm.try_invoke_durable_guest_result(b, work.ticket, ores_guest_entry, 1);
        assert(receipt && receipt->outcome.status == ORES_GUEST_OK);
        assert(receipt->outcome.value == i+1);
    }
    assert(vm.invoke_guest(a, ores_guest_entry, 0) == 1000);
    assert(vm.invoke_guest(b, ores_guest_entry, 0) == 500);
    // Compiler-generated overflow returns a typed failure, not a partially
    // committed state or a stuck durable ticket.
    auto overflow = vm.enqueue_durable(a, a_actor, 7, 1001, 1);
    auto result = vm.try_invoke_durable_guest_result(a, overflow.ticket, ores_guest_entry, INT64_MAX);
    assert(result && result->outcome.status == ORES_GUEST_OVERFLOW);
    assert(vm.durable_snapshot(a).entries == 0);
    assert(vm.invoke_guest(a, ores_guest_entry, 0) == 1000);
    // A second lock admits another actor while preserving actor nonreentrancy.
    auto a_actor2 = vm.register_actor(a, 2);
    auto other = vm.enqueue_durable(a, a_actor2, 19, 1002, 1);
    auto other_result = vm.try_invoke_durable_guest_result(a, other.ticket, ores_guest_entry, 1);
    assert(other_result && other_result->outcome.value == 1001);
    vm.retire_actor(a, a_actor);
    vm.retire_actor(a, a_actor2);
    vm.retire_actor(b, b_actor);
    vm.begin_shutdown(a); vm.complete_shutdown(a); vm.destroy(a);
    vm.begin_shutdown(b); vm.complete_shutdown(b); vm.destroy(b);
}
