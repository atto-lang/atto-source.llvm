#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <cstdint>
extern "C" int64_t guest_counter_llvm(OresIsolateContext*, int64_t);
int main() {
    oreslang::runtime::OresVM vm;
    auto x = vm.create_isolate({"x", 4096, 2});
    auto y = vm.create_isolate({"y", 4096, 2});
    vm.start(x); vm.start(y);
    assert(vm.invoke_guest(x, guest_counter_llvm, 5) == 5);
    assert(vm.invoke_guest(y, guest_counter_llvm, 100) == 100);
    assert(vm.invoke_guest(x, guest_counter_llvm, 1) == 6);
    assert(vm.invoke_guest(y, guest_counter_llvm, 1) == 101);
    vm.begin_shutdown(x); vm.complete_shutdown(x); vm.destroy(x);
    vm.begin_shutdown(y); vm.complete_shutdown(y); vm.destroy(y);
}
