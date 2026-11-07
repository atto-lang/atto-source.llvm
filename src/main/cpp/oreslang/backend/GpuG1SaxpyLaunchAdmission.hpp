#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace oreslang::gpu::g1 {

// G1 lowers f32 buffers to NVPTX with a four-byte element ABI.
static_assert(sizeof(float) == 4, "GPU-G1 requires 32-bit f32 storage");

// Structural preflight ONLY. These fields must be constructed from trusted
// allocator/queue records; user-supplied IDs are not security capabilities.
enum class Loan : std::uint8_t { ReadOnly, ExclusiveWrite };

struct DeviceView {
    std::uint64_t tenant = 0;
    std::uint64_t device = 0;
    std::uint64_t device_generation = 0;
    std::uint64_t allocation = 0;
    std::uint64_t allocation_generation = 0;
    std::uint64_t allocation_elements = 0;
    std::uint64_t first = 0;
    std::uint64_t length = 0;
    Loan loan = Loan::ReadOnly;
};

struct QueueLocation {
    std::uint64_t tenant = 0;
    std::uint64_t device = 0;
    std::uint64_t device_generation = 0;
    std::uint64_t queue = 0;
    std::uint64_t queue_generation = 0;
};

struct LaunchLimits {
    std::uint64_t maximum_threads_per_group = 0;
    std::uint64_t maximum_groups_x = 0;
    std::uint64_t maximum_buffer_bytes = 0;
};

struct LaunchPlan {
    std::uint64_t logical_elements;
    std::uint64_t group_threads;
    std::uint64_t groups_x;
    std::uint64_t physical_threads;
    bool empty;
};

inline void require_view(const DeviceView& view, Loan expected,
                         const QueueLocation& queue, const LaunchLimits& caps) {
    if (!view.tenant || !view.device || !view.device_generation ||
        !view.allocation || !view.allocation_generation ||
        view.tenant != queue.tenant || view.device != queue.device ||
        view.device_generation != queue.device_generation ||
        view.loan != expected)
        throw std::invalid_argument("GPU-G1-ADMISSION-001: device identity or loan mismatch");
    if (view.allocation_elements > caps.maximum_buffer_bytes / sizeof(float) ||
        view.first > view.allocation_elements ||
        view.length > view.allocation_elements - view.first)
        throw std::invalid_argument("GPU-G1-ADMISSION-002: device span exceeds allocation budget");
}

inline bool overlaps(const DeviceView& a, const DeviceView& b) {
    if (a.allocation != b.allocation) return false;
    // An allocation ID from a different generation is never valid as an
    // independent reusable allocation while the old generation is in flight.
    if (a.allocation_generation != b.allocation_generation ||
        a.allocation_elements != b.allocation_elements)
        throw std::invalid_argument("GPU-G1-ADMISSION-003: stale or contradictory allocation identity");
    if (a.length == 0 || b.length == 0) return false;
    // require_view already proved these sums are within allocation_elements.
    return a.first < b.first + b.length && b.first < a.first + a.length;
}

inline LaunchPlan admit_saxpy_launch(const DeviceView& x, const DeviceView& y,
                                    const DeviceView& out,
                                    const QueueLocation& queue,
                                    const LaunchLimits& caps,
                                    std::uint64_t n, std::uint64_t group_threads) {
    if (!queue.tenant || !queue.device || !queue.device_generation ||
        !queue.queue || !queue.queue_generation)
        throw std::invalid_argument("GPU-G1-ADMISSION-001: unauthenticated queue identity");
    if (!caps.maximum_buffer_bytes || !caps.maximum_threads_per_group ||
        !caps.maximum_groups_x || !group_threads ||
        group_threads > caps.maximum_threads_per_group ||
        group_threads > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("GPU-G1-ADMISSION-004: invalid group or capability");
    require_view(x, Loan::ReadOnly, queue, caps);
    require_view(y, Loan::ReadOnly, queue, caps);
    require_view(out, Loan::ExclusiveWrite, queue, caps);
    if (n > x.length || n > y.length || n > out.length)
        throw std::invalid_argument("GPU-G1-ADMISSION-002: logical length exceeds device view");
    // Shared READ views may overlap; an in-flight EXCLUSIVE WRITE may not
    // overlap either input. The provider must still atomically claim loans.
    if (overlaps(x, out) || overlaps(y, out))
        throw std::invalid_argument("GPU-G1-ADMISSION-005: overlapping writable footprint");
    (void)overlaps(x, y);  // Reject stale IDs without forbidding shared reads.
    // Account each distinct resident allocation exactly once, including
    // aliased read-only views and disjoint read/write ranges in one allocation.
    // All views with one allocation ID MUST agree on its generation and
    // full allocation extent. This is a quota check, not an allocator lease.
    const DeviceView* views[] = {&x, &y, &out};
    const std::uint64_t max_elements = caps.maximum_buffer_bytes / sizeof(float);
    std::uint64_t resident_elements = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        bool counted = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (views[i]->allocation != views[j]->allocation) continue;
            // Pairwise overlaps() above has validated alias generations and
            // allocation extents, regardless of their byte-range overlap.
            counted = true;
        }
        if (counted) continue;
        const auto count = views[i]->allocation_elements;
        if (count > max_elements - resident_elements)
            throw std::invalid_argument("GPU-G1-ADMISSION-006: total resident device allocation quota exceeded");
        resident_elements += count;
    }
    const std::uint64_t groups = n / group_threads + (n % group_threads != 0);
    // CUDA Driver API uses unsigned int for gridDimX and blockDimX; the
    // NVPTX backend reads 32-bit CTA/block coordinates. Even a provider
    // advertising an oversized limit may not authorize truncation here.
    if (groups > std::numeric_limits<std::uint32_t>::max() ||
        groups > caps.maximum_groups_x ||
        (groups != 0 && group_threads > std::numeric_limits<std::uint64_t>::max() / groups))
        throw std::invalid_argument("GPU-G1-ADMISSION-004: physical launch overflow");
    return {n, group_threads, groups, groups * group_threads, n == 0};
}

} // namespace oreslang::gpu::g1
