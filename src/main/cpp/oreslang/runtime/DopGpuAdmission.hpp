#pragma once

#include "oreslang/runtime/DopKernelIR.hpp"
#include "oreslang/runtime/GpuBufferFence.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace oreslang::dop {

// Trusted-provider descriptor. Byte length alone never establishes whether
// storage is AoS, SoA or AoSoA; an equal-size, wrong-layout buffer is invalid.
struct DeviceBuffer {
    Plan layout;
    gpu::BufferFence& fence;

    DeviceBuffer(Plan shape, gpu::BufferFence& owner)
        : layout(std::move(shape)), fence(owner) {
        const auto descriptor = fence.snapshot();
        if (layout.rows() == 0 || layout.byte_length() == 0
                || descriptor.bytes != layout.byte_length())
            throw std::invalid_argument("DOP GPU device allocation and layout disagree");
    }
};

struct GpuBinding {
    Plan layout;
    OpenClArtifact artifact;
    gpu::Identity input;
    gpu::Identity output;
    std::uint64_t bytes_per_buffer;
    std::uint64_t total_reserved_bytes;
};

// This is a checked PRE-DISPATCH PLAN, not an allocation, asynchronous
// transfer, device execution, ownership move or security capability.
// The real backend must atomically revalidate while admitting ownership and
// hold each fence until authenticated device completion and physical release.
inline GpuBinding preflight_gpu(
        const Kernel& kernel, GpuTarget target,
        const DeviceBuffer& input, const DeviceBuffer& output) {
    if (&input.fence == &output.fence)
        throw std::invalid_argument("DOP GPU input/output owners must be distinct");
    const auto artifact = lower_opencl(kernel, target);
    const auto& layout = kernel.layout;
    const auto bytes = layout.byte_length();
    if (layout.rows() == 0 || bytes == 0 || artifact.required_bytes != bytes)
        throw std::invalid_argument("DOP GPU requires nonempty exact layout");
    if (!matching_layout(layout, input.layout)
            || !matching_layout(layout, output.layout))
        throw std::invalid_argument("DOP GPU kernel and buffers require identical layouts");

    // Separate input and output device allocations require two reservations.
    const auto total = checked_mul(bytes, 2);
    if (total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("DOP GPU reservation exceeds signed 64-bit ABI");
    const auto src = input.fence.snapshot();
    const auto dst = output.fence.snapshot();
    if (src.status != gpu::Status::Idle || dst.status != gpu::Status::Idle)
        throw std::logic_error("DOP GPU buffer not idle");
    if (src.bytes != bytes || dst.bytes != bytes)
        throw std::invalid_argument("DOP GPU buffer bytes must exactly match padded layout");
    if (src.identity.allocation_generation == 0
            || dst.identity.allocation_generation == 0
            || src.identity.allocation_generation == dst.identity.allocation_generation
            || src.identity.tenant != dst.identity.tenant
            || src.identity.device != dst.identity.device)
        throw std::invalid_argument(
                "DOP GPU requires same tenant/device and distinct nonzero generations");
    return GpuBinding{layout, artifact, src.identity, dst.identity, bytes, total};
}

} // namespace oreslang::dop
