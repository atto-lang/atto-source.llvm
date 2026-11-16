#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace oreslang::gpu {

// Host-side geometry arithmetic only. A successful plan grants no GPU
// authority, device loan, provider handle or completed kernel execution.
struct Dim3 {
    std::int64_t x;
    std::int64_t y;
    std::int64_t z;
    Dim3(std::int64_t x_, std::int64_t y_, std::int64_t z_)
        : x(x_), y(y_), z(z_) {
        if (x < 0 || y < 0 || z < 0)
            throw std::invalid_argument("GPU-LAUNCH-003: dimensions cannot be negative");
    }
};

struct Limits {
    Dim3 max_grid;
    Dim3 max_block;
    std::int64_t max_threads_per_group;
    std::int64_t max_shared_bytes;

    Limits(Dim3 max_grid_, Dim3 max_block_,
           std::int64_t max_threads_, std::int64_t max_shared_)
        : max_grid(max_grid_), max_block(max_block_),
          max_threads_per_group(max_threads_), max_shared_bytes(max_shared_) {
        if (max_grid.x <= 0 || max_grid.y <= 0 || max_grid.z <= 0 ||
            max_block.x <= 0 || max_block.y <= 0 || max_block.z <= 0 ||
            max_threads_per_group <= 0 || max_shared_bytes < 0)
            throw std::invalid_argument("GPU-LAUNCH-003: invalid device geometry limits");
    }
};

struct LaunchPlan {
    Dim3 grid;
    Dim3 block;
    std::int64_t shared_bytes;
    std::int64_t total_groups;
    std::int64_t threads_per_group;
    std::int64_t total_dispatched_lanes;
    bool empty_launch;
};

inline std::int64_t checked_product(std::int64_t a, std::int64_t b,
                                    const char* kind) {
    if (a < 0 || b < 0 || (a != 0 && b > std::numeric_limits<std::int64_t>::max() / a))
        throw std::invalid_argument(std::string("GPU-LAUNCH-003: ") + kind +
                                    " product overflows signed 64-bit");
    return a * b;
}

// Limits must originate from an authenticated provider, not an untrusted guest
// descriptor. This verifier does not dispatch and must not mint fence tickets.
inline LaunchPlan validate_launch_internal(Dim3 grid, Dim3 block,
                                           std::int64_t shared_bytes,
                                           const Limits& limits,
                                           bool allow_empty_logical_grid) {
    if (block.x == 0 || block.y == 0 || block.z == 0)
        throw std::invalid_argument("GPU-LAUNCH-003: block dimensions must be positive");
    if (!allow_empty_logical_grid && (grid.x == 0 || grid.y == 0 || grid.z == 0))
        throw std::invalid_argument("GPU-LAUNCH-003: physical grid dimensions must be positive");
    if (grid.x > limits.max_grid.x || grid.y > limits.max_grid.y ||
        grid.z > limits.max_grid.z || block.x > limits.max_block.x ||
        block.y > limits.max_block.y || block.z > limits.max_block.z)
        throw std::invalid_argument("GPU-LAUNCH-003: launch exceeds device axis limit");
    if (shared_bytes < 0 || shared_bytes > limits.max_shared_bytes)
        throw std::invalid_argument("GPU-LAUNCH-003: launch exceeds shared memory budget");

    const auto threads = checked_product(
        checked_product(block.x, block.y, "block"), block.z, "block");
    if (threads > limits.max_threads_per_group)
        throw std::invalid_argument("GPU-LAUNCH-003: block exceeds threads-per-group budget");

    // Short-circuit any zero axis BEFORE multiplying potentially huge values.
    const bool empty = grid.x == 0 || grid.y == 0 || grid.z == 0;
    const auto groups = empty ? std::int64_t{0} : checked_product(
        checked_product(grid.x, grid.y, "grid"), grid.z, "grid");
    const auto total = checked_product(groups, threads, "total launch");
    return {grid, block, shared_bytes, groups, threads, total, empty};
}

// Exact physical geometry cannot have zero axes. An empty logical extent
// uses a separate quiescent internal plan and must never be enqueued.
inline LaunchPlan validate_launch(Dim3 grid, Dim3 block,
                                  std::int64_t shared_bytes, const Limits& limits) {
    return validate_launch_internal(grid, block, shared_bytes, limits, false);
}

// The logical extent is not overwritten by physical grid derivation: later
// device lowering must prove padded-tail memory safety and barrier convergence.
struct LogicalLaunchPlan {
    Dim3 over;
    Dim3 group;
    std::int64_t logical_work_items;
    LaunchPlan physical;
};

inline std::int64_t ceil_div_nonnegative(std::int64_t extent, std::int64_t group) {
    if (group <= 0)
        throw std::invalid_argument("GPU-LAUNCH-003: group dimensions must be positive");
    // No (extent + group - 1): this would overflow valid 64-bit extents.
    return extent / group + (extent % group == 0 ? 0 : 1);
}

inline LogicalLaunchPlan validate_logical_launch(Dim3 over, Dim3 group,
                                                 std::int64_t shared_bytes,
                                                 const Limits& limits) {
    const Dim3 grid{ceil_div_nonnegative(over.x, group.x),
                    ceil_div_nonnegative(over.y, group.y),
                    ceil_div_nonnegative(over.z, group.z)};
    const auto physical = validate_launch_internal(grid, group, shared_bytes, limits, true);
    const bool empty = over.x == 0 || over.y == 0 || over.z == 0;
    const auto logical_items = empty ? std::int64_t{0}
            : checked_product(checked_product(over.x, over.y, "logical"),
                              over.z, "logical");
    return {over, group, logical_items, physical};
}

} // namespace oreslang::gpu
