#include "oreslang/runtime/GpuLaunchGeometry.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <iostream>

using oreslang::gpu::Dim3;
using oreslang::gpu::Limits;
using oreslang::gpu::validate_launch;

static int checked = 0;
static void verify(bool ok) {
    if (!ok) throw std::runtime_error("GPU launch geometry assertion failed");
    ++checked;
}
template <class F> static void rejects(F fn) {
    try { fn(); }
    catch (const std::invalid_argument&) { ++checked; return; }
    throw std::runtime_error("GPU launch geometry accepted invalid input");
}

int main() {
    const Limits caps{{65535, 65535, 65535}, {1024, 1024, 64}, 1024, 49152};
    const auto plan = validate_launch({10, 3, 2}, {8, 4, 2}, 4096, caps);
    verify(plan.total_groups == 60 && plan.threads_per_group == 64 &&
           plan.total_logical_threads == 3840 && !plan.empty_launch);
    verify(validate_launch({0, 1, 1}, {8, 1, 1}, 0, caps).empty_launch);
    rejects([&] { (void)validate_launch({1, 1, 1}, {0, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({1, 1, 1}, {1025, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({1, 1, 1}, {64, 32, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({65536, 1, 1}, {1, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({1, 1, 1}, {1, 1, 1}, 49153, caps); });
    rejects([&] { (void)validate_launch({1, 1, 1}, {1, 1, 1}, -1, caps); });
    rejects([] { (void)Dim3{-1, 1, 1}; });
    rejects([] { (void)Limits{{0, 1, 1}, {1, 1, 1}, 1, 0}; });
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const Limits huge{{maximum, maximum, 3}, {2, 2, 2}, 8, 0};
    rejects([&] { (void)validate_launch({maximum, 2, 1}, {1, 1, 1}, 0, huge); });
    rejects([&] { (void)validate_launch({maximum, 1, 1}, {2, 1, 1}, 0, huge); });
    verify(validate_launch({maximum, maximum, 0}, {2, 1, 1}, 0, huge).empty_launch);
    std::cout << "PASS " << checked << " native host-only GPU geometry assertions\n";
    return 0;
}
