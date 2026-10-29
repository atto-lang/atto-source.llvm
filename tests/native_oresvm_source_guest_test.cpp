#include "oreslang/runtime/OresVM.hpp"
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
extern "C" std::int64_t ores_guest_entry(OresIsolateContext*, std::int64_t);
int main() {
    oreslang::runtime::OresVM vm;
    auto a = vm.create_isolate({"tenant-a", 4096, 3});
    auto b = vm.create_isolate({"tenant-b", 4096, 3});
    vm.start(a); vm.start(b);
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
    vm.begin_shutdown(a); vm.complete_shutdown(a); vm.destroy(a);
    vm.begin_shutdown(b); vm.complete_shutdown(b); vm.destroy(b);
}
