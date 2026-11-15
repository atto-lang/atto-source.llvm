#include "oreslang/backend/GpuG1SaxpyLaunchAdmission.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool test) { ++checks; if (!test) std::abort(); }
template <class Func> void reject(Func&& func) {
    bool threw = false;
    try { func(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw);
}
}
int main() {
    using namespace oreslang::gpu::g1;
    const QueueLocation queue{7, 8, 9, 10, 11};
    const LaunchLimits caps{1024, 65535, 1048576};
    const DeviceView x{7, 8, 9, 21, 1, 1000, 0, 1000, Loan::ReadOnly};
    const DeviceView y{7, 8, 9, 22, 1, 1000, 0, 1000, Loan::ReadOnly};
    const DeviceView out{7, 8, 9, 23, 1, 1000, 0, 1000, Loan::ExclusiveWrite};
    const auto ok = admit_saxpy_launch(x, y, out, queue, caps, 1000, 256);
    check(ok.logical_elements == 1000);
    check(ok.groups_x == 4);
    check(ok.physical_threads == 1024);
    check(!ok.empty);
    const auto empty = admit_saxpy_launch(x, y, out, queue, caps, 0, 256);
    check(empty.groups_x == 0 && empty.empty);
    const auto exact = admit_saxpy_launch(x, y, out, queue, caps, 256, 256);
    check(exact.groups_x == 1 && exact.physical_threads == 256);
    const auto one = admit_saxpy_launch(x, y, out, queue, caps, 1, 256);
    check(one.groups_x == 1 && one.logical_elements == 1);
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, caps, 1001, 256); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, caps, 1, 0); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, caps, 1, 2048); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, {7,8,0,10,11}, caps, 1, 256); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, {7,8,9,0,11}, caps, 1, 256); });
    auto changed = out;
    changed.tenant = 99;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.device = 99;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.device_generation = 99;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation_generation = 0;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.loan = Loan::ReadOnly;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation_elements = 10; changed.first = 9; changed.length = 2;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation_elements = 300000;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation = x.allocation;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation = y.allocation;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation = x.allocation; changed.allocation_generation = 2;
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    changed = out; changed.allocation = x.allocation;
    changed.first = 500; changed.length = 500;
    // A disjoint exclusive view in the SAME allocation is valid only if
    // the read loan is also restricted to a demonstrably disjoint footprint.
    auto read_prefix = x; read_prefix.length = 500;
    check(admit_saxpy_launch(read_prefix, y, changed, queue, caps, 500, 256).groups_x == 2);
    // Read aliases are allowed, since no write footprint is shared.
    auto shared_y = x;
    check(admit_saxpy_launch(x, shared_y, out, queue, caps, 7, 256).groups_x == 1);
    shared_y.allocation_generation = 5;
    reject([&] { (void)admit_saxpy_launch(x, shared_y, out, queue, caps, 1, 256); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, {1024,3,1048576}, 1000, 256); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, {0,100,1048576}, 1, 256); });
    reject([&] { (void)admit_saxpy_launch(x, y, out, queue, {1024,100,0}, 1, 256); });
    changed = out; changed.first = std::numeric_limits<std::uint64_t>::max();
    reject([&] { (void)admit_saxpy_launch(x, y, changed, queue, caps, 1, 256); });
    std::cout << "PASS " << checks << " host-only GPU-G1 structural admission assertions\n";
}
