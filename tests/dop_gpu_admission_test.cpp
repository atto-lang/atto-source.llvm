#include "oreslang/runtime/DopGpuAdmission.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

static void check(bool condition) {
    if (!condition) throw std::runtime_error("DOP GPU admission assertion failed");
}
template<class E, class F> static void expect(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("missing DOP GPU admission rejection");
}

int main() {
    using namespace oreslang::dop;
    using oreslang::gpu::BufferFence;
    using oreslang::gpu::Identity;
    for (const auto kind : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        const Plan plan(kind, 7, 3, 4, 16, kind == Layout::AoSoA ? 4u : 1u);
        const Kernel kernel(plan, Element::F32, Op::ScaleBias, 2.0f, 1.0f);
        BufferFence src(Identity{"tenant", "device-0", 7}, plan.byte_length());
        BufferFence dst(Identity{"tenant", "device-0", 8}, plan.byte_length());
        const DeviceBuffer input(plan, src), output(plan, dst);
        const auto binding = preflight_gpu(kernel, {64, 8192, 100, true}, input, output);
        check(matching_layout(binding.layout, plan));
        check(binding.bytes_per_buffer == plan.byte_length());
        check(binding.total_reserved_bytes == plan.byte_length() * 2);
        check(binding.artifact.required_bytes == plan.byte_length());
        check(binding.artifact.global_rows == 7 && binding.artifact.global_fields == 3);
        check(binding.input.allocation_generation != binding.output.allocation_generation);
    }

    // Equal padded byte lengths do not imply equal field offsets.
    const Plan aos(Layout::AoS, 2, 2, 4, 8, 1);
    const Plan soa(Layout::SoA, 2, 2, 4, 8, 1);
    check(aos.byte_length() == soa.byte_length());
    check(aos.byte_offset(0, 1) != soa.byte_offset(0, 1));
    const Kernel kernel(aos, Element::F32, Op::Scale, 2, 0);
    BufferFence a(Identity{"t", "d", 1}, aos.byte_length());
    BufferFence b(Identity{"t", "d", 2}, aos.byte_length());
    const DeviceBuffer input(aos, a), output(aos, b), wrong_layout(soa, b);
    const GpuTarget target{64, 8192, 100, true};
    expect<std::invalid_argument>([&] { (void)preflight_gpu(kernel, target, wrong_layout, output); });
    expect<std::invalid_argument>([&] { (void)preflight_gpu(kernel, target, input, wrong_layout); });
    expect<std::invalid_argument>([&] { (void)preflight_gpu(kernel, target, input, input); });
    expect<std::invalid_argument>([&] {
        (void)preflight_gpu(kernel, target, input, DeviceBuffer(aos, a));
    });
    expect<std::invalid_argument>([&] {
        BufferFence wrong_bytes(Identity{"t", "d", 3}, aos.byte_length() + 4);
        (void)DeviceBuffer(aos, wrong_bytes);
    });
    for (const auto& foreign : {Identity{"other", "d", 3}, Identity{"t", "other", 3},
                               Identity{"t", "d", 1}, Identity{"t", "d", 0}}) {
        BufferFence foreign_fence(foreign, aos.byte_length());
        const DeviceBuffer candidate(aos, foreign_fence);
        expect<std::invalid_argument>([&] {
            (void)preflight_gpu(kernel, target, input, candidate);
        });
    }
    expect<std::invalid_argument>([&] {
        (void)preflight_gpu(kernel, {32, 8192, 100, true}, input, output);
    });
    expect<std::invalid_argument>([&] {
        (void)preflight_gpu(kernel, {64, 1, 100, true}, input, output);
    });

    const auto ticket = a.begin_command(Identity{"t", "d", 1});
    expect<std::logic_error>([&] { (void)preflight_gpu(kernel, target, input, output); });
    a.cancel_pending(ticket);
    expect<std::logic_error>([&] { (void)preflight_gpu(kernel, target, input, output); });
    a.acknowledge_cancelled_device_event(ticket, Identity{"t", "d", 1});
    a.retire_after_backend_release(Identity{"t", "d", 1});
    a.close_after_retirement();

    const Plan empty(Layout::SoA, 0, 1, 4, 8, 1);
    const Kernel empty_kernel(empty, Element::F32, Op::Scale, 2, 0);
    expect<std::invalid_argument>([&] {
        (void)lower_opencl(empty_kernel, {64, 8192, 100, true});
    });
    expect<std::invalid_argument>([&] {
        BufferFence placeholder(Identity{"t", "d", 11}, 4);
        (void)DeviceBuffer(empty, placeholder);
    });

    // Native uint64_t can represent two buffers that Java long cannot:
    // require signed-64 ABI parity, without actually allocating huge storage.
    const Plan huge(Layout::AoS,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 8 + 1,
            1, 4, 4, 1);
    BufferFence huge_a(Identity{"t", "d", 100}, huge.byte_length());
    BufferFence huge_b(Identity{"t", "d", 101}, huge.byte_length());
    const DeviceBuffer ha(huge, huge_a), hb(huge, huge_b);
    const Kernel huge_kernel(huge, Element::F32, Op::Scale, 2, 0);
    expect<std::overflow_error>([&] {
        (void)preflight_gpu(huge_kernel, {64,
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(), true}, ha, hb);
    });

    std::cout << "typed DOP GPU placement admission tests passed\n";
}
