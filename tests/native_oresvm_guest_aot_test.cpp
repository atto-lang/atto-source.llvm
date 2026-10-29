#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <cstdint>
extern "C" int32_t guest_counter_llvm(OresIsolateContext*, int64_t, int64_t*);
int main() {
    oreslang::runtime::OresVM vm;
    auto x = vm.create_isolate({"x", 4096, 2});
    auto y = vm.create_isolate({"y", 4096, 2});
    vm.start(x); vm.start(y);
    assert(vm.invoke_guest(x, guest_counter_llvm, 5) == 5);
    assert(vm.invoke_guest(y, guest_counter_llvm, 100) == 100);
    assert(vm.invoke_guest(x, guest_counter_llvm, 1) == 6);
    assert(vm.invoke_guest(y, guest_counter_llvm, 1) == 101);
    assert(vm.invoke_guest(x, guest_counter_llvm, -7) == -1);
    assert(vm.invoke_guest_result(x, guest_counter_llvm, 0).status == ORES_GUEST_OK);
    // Overflow is an isolate ABI error, not silent modular state corruption.
    const auto overflow = vm.invoke_guest_result(y, guest_counter_llvm, INT64_MAX);
    assert(overflow.status == ORES_GUEST_OVERFLOW && overflow.value == 0);
    assert(vm.invoke_guest(y, guest_counter_llvm, 0) == 101);
    vm.begin_shutdown(x); vm.complete_shutdown(x); vm.destroy(x);
    vm.begin_shutdown(y); vm.complete_shutdown(y); vm.destroy(y);
}
