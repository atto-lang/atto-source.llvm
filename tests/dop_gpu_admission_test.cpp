#include "oreslang/runtime/DopGpuAdmission.hpp"
#include <cstdint>
#include <atomic>
#include <thread>
#include <optional>
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


    // Claiming typed DOP buffers starts both fences together.
    {
        const Plan shape(Layout::AoSoA, 7, 3, 4, 16, 4);
        const Kernel operation(shape, Element::F32, Op::Scale, 2.0f, 0.0f);
        BufferFence source(Identity{"t", "gpu", 10}, shape.byte_length());
        BufferFence result(Identity{"t", "gpu", 11}, shape.byte_length());
        DeviceBuffer in(shape, source), out(shape, result);
        auto admitted = claim_gpu(operation, {64, 8192, 100, true}, in, out);
        check(admitted.binding.bytes_per_buffer == shape.byte_length());
        check(source.status() == oreslang::gpu::Status::InFlight);
        check(result.status() == oreslang::gpu::Status::InFlight);
        expect<std::logic_error>([&] { (void)source.take(); });
        expect<std::logic_error>([&] { (void)result.take(); });
        source.complete(admitted.tickets.input_ticket, true);
        result.complete(admitted.tickets.output_ticket, true);
        check(source.status() == oreslang::gpu::Status::Idle);
        check(result.status() == oreslang::gpu::Status::Idle);
    }

    // If either buffer is busy, a rejected pair claim never changes the other.
    {
        BufferFence available(Identity{"t", "gpu", 20}, 64);
        BufferFence busy(Identity{"t", "gpu", 21}, 64);
        auto original = busy.begin_command(Identity{"t", "gpu", 21});
        expect<std::logic_error>([&] { (void)BufferFence::claim_pair(available, busy, 64); });
        check(available.status() == oreslang::gpu::Status::Idle);
        check(busy.status() == oreslang::gpu::Status::InFlight);
        busy.complete(original, true);
        expect<std::invalid_argument>([&] { (void)BufferFence::claim_pair(available, busy, 16); });
        check(available.status() == oreslang::gpu::Status::Idle);
        check(busy.status() == oreslang::gpu::Status::Idle);
        auto moved = busy.take();
        expect<std::logic_error>([&] { (void)BufferFence::claim_pair(available, busy, 64); });
        auto claimed = BufferFence::claim_pair(available, moved, 64);
        available.cancel_pending(claimed.input_ticket);
        moved.cancel_pending(claimed.output_ticket);
        expect<std::logic_error>([&] { (void)BufferFence::claim_pair(available, moved, 64); });
        available.acknowledge_cancelled_device_event(claimed.input_ticket, claimed.input);
        moved.acknowledge_cancelled_device_event(claimed.output_ticket, claimed.output);
        available.retire_after_backend_release(claimed.input);
        moved.retire_after_backend_release(claimed.output);
    }

    // Opposite-order concurrent claims must never deadlock or half-claim.
    for (int iteration = 0; iteration < 100; ++iteration) {
        BufferFence left(Identity{"t", "gpu", 50}, 64);
        BufferFence right(Identity{"t", "gpu", 51}, 64);
        std::atomic<bool> start{false};
        std::atomic<int> wins{0}, rejects{0};
        std::optional<BufferFence::PairTickets> results[2];
        auto attempt = [&](int slot, BufferFence& input, BufferFence& output) {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                results[slot] = BufferFence::claim_pair(input, output, 64);
                wins.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::logic_error&) {
                rejects.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(attempt, 0, std::ref(left), std::ref(right));
        std::thread t2(attempt, 1, std::ref(right), std::ref(left));
        start.store(true, std::memory_order_release);
        t1.join();
        t2.join();
        check(wins == 1 && rejects == 1);
        check(left.status() == oreslang::gpu::Status::InFlight);
        check(right.status() == oreslang::gpu::Status::InFlight);
        const auto& winner = results[0].has_value() ? *results[0] : *results[1];
        BufferFence& src = results[0].has_value() ? left : right;
        BufferFence& dst = results[0].has_value() ? right : left;
        src.complete(winner.input_ticket, true);
        dst.complete(winner.output_ticket, true);
    }

    std::cout << "typed DOP GPU placement admission tests passed\n";
}
