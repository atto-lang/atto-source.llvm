#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
extern "C" std::int32_t ores_guest_entry(OresIsolateContext*, std::int64_t, std::int64_t*);
int main() {
    oreslang::runtime::OresVM vm;
    auto a = vm.create_isolate({"tenant-a", 4096, 3});
    auto b = vm.create_isolate({"tenant-b", 4096, 3});
    vm.start(a); vm.start(b);
    // -1 and -2 are valid signed scalar results, not transport errors.
    assert(vm.invoke_guest(a, ores_guest_entry, -1) == -1);
    assert(vm.invoke_guest(a, ores_guest_entry, -2) == -3);
    assert(vm.invoke_guest(a, ores_guest_entry, 3) == 0);
    const auto zero = vm.invoke_guest_result(a, ores_guest_entry, 0);
    assert(zero.status == ORES_GUEST_OK && zero.value == 0);
    assert(vm.invoke_guest(a, ores_guest_entry, 5) == 5);
    assert(vm.invoke_guest(b, ores_guest_entry, 100) == 100);
    assert(vm.invoke_guest(a, ores_guest_entry, 4) == 9);
    assert(vm.invoke_guest(b, ores_guest_entry, 2) == 102);
    std::vector<std::thread> workers;
    for (int n = 0; n < 4; ++n) workers.emplace_back([&] {
        for (int i = 0; i < 250; ++i)
            (void)vm.invoke_guest(a, ores_guest_entry, 1);
    });
    for (auto& t : workers) t.join();
    assert(vm.invoke_guest(a, ores_guest_entry, 0) == 1009);
    assert(vm.invoke_guest(b, ores_guest_entry, 0) == 102);
    // The source-generated wrapper must return a recoverable ABI failure,
    // never llvm.trap, wrapping signed i64 or publishing a partial state.
    const auto overflow = vm.invoke_guest_result(a, ores_guest_entry, INT64_MAX);
    assert(overflow.status == ORES_GUEST_OVERFLOW && overflow.value == 0);
    assert(vm.invoke_guest(a, ores_guest_entry, 0) == 1009);
    assert(vm.invoke_guest(b, ores_guest_entry, INT64_MIN) == INT64_MIN + 102);
    const auto underflow = vm.invoke_guest_result(b, ores_guest_entry, -103);
    assert(underflow.status == ORES_GUEST_OVERFLOW && underflow.value == 0);
    assert(vm.invoke_guest(b, ores_guest_entry, 0) == INT64_MIN + 102);
    assert(vm.invoke_guest(a, ores_guest_entry, 0) == 1009);
    vm.begin_shutdown(a); vm.complete_shutdown(a); vm.destroy(a);
    vm.begin_shutdown(b); vm.complete_shutdown(b); vm.destroy(b);
}
