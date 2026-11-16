#include "oreslang/runtime/GpuLaunchGeometry.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <iostream>

using oreslang::gpu::Dim3;
using oreslang::gpu::Limits;
using oreslang::gpu::validate_launch;
using oreslang::gpu::validate_logical_launch;

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
           plan.total_dispatched_lanes == 3840 && !plan.empty_launch);
    rejects([&] { (void)validate_launch({0, 1, 1}, {8, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({1, 0, 1}, {8, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_launch({1, 1, 0}, {8, 1, 1}, 0, caps); });
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
    verify(validate_logical_launch({maximum, maximum, 0},
                                   {2, 1, 1}, 0, huge).physical.empty_launch);
    const auto logical = validate_logical_launch({1000, 1, 1},
                                                   {256, 1, 1}, 0, caps);
    verify(logical.logical_work_items == 1000 && logical.physical.grid.x == 4
           && logical.physical.total_dispatched_lanes == 1024);
    const auto logical2d = validate_logical_launch({35, 17, 1},
                                                    {16, 16, 1}, 0, caps);
    verify(logical2d.logical_work_items == 595
           && logical2d.physical.grid.x == 3
           && logical2d.physical.grid.y == 2
           && logical2d.physical.total_dispatched_lanes == 1536);
    const auto logical_zero = validate_logical_launch({0, 65535, 65535},
                                                       {1, 1, 1}, 0, caps);
    verify(logical_zero.logical_work_items == 0
           && logical_zero.physical.empty_launch);
    rejects([&] { (void)validate_logical_launch({8, 1, 1},
                                                  {0, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_logical_launch({0, 65536, 1},
                                                  {1, 1, 1}, 0, caps); });
    rejects([&] { (void)validate_logical_launch({0, 1, 1},
                                                  {1, 1, 1}, 49153, caps); });
    rejects([&] { (void)validate_logical_launch({8, 1, 1},
                                                  {1, 1, 1}, 49153, caps); });
    rejects([&] { (void)validate_logical_launch({maximum, 1, 1},
                                                  {2, 1, 1}, 0, huge); });
    std::cout << "PASS " << checked << " native host-only GPU geometry assertions\n";
    return 0;
}
