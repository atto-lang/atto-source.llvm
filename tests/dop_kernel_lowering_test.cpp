#include "oreslang/runtime/DopKernelIR.hpp"

#include <cmath>
#include <atomic>
#include <thread>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <string>

static void check(bool condition) {
    if (!condition) throw std::runtime_error("typed DOP kernel test failed");
}
template<class Exception, class F>
static void expect(F fn) {
    try { fn(); } catch (const Exception&) { return; }
    throw std::runtime_error("expected DOP contract failure");
}

int main() {
    using namespace oreslang::dop;
    for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        const auto lanes = layout == Layout::AoSoA ? 4u : 1u;
        Plan p(layout, 7, 3, 4, 16, lanes);
        Kernel k(p, Element::F32, Op::ScaleBias, 2.0f, 1.0f);
        HostBuffer src(p), dst(p);
        for (std::uint64_t row = 0; row < 7; ++row)
            for (std::uint32_t field = 0; field < 3; ++field)
                src.write(row, field, static_cast<float>(row * 3 + field) * 0.5f);
        execute_cpu(k, src, dst);
        for (std::uint64_t row = 0; row < 7; ++row)
            for (std::uint32_t field = 0; field < 3; ++field)
                check(dst.read(row, field) == static_cast<float>(row * 3 + field) + 1.0f);
        auto generated = lower_opencl(k, {64, 4096, 100, true});
        check(generated.required_bytes == p.byte_length());
        check(generated.global_rows == 7 && generated.global_fields == 3);
        check(generated.device_source.find("__kernel void oreslang_dop_f32") != std::string::npos);
        check(generated.device_source.find("get_global_id(1)") != std::string::npos);
        check(generated.device_source.find("dst[slot]") != std::string::npos);
        std::cout << "GPU SOURCE " << static_cast<int>(layout) << "\n"
                  << generated.device_source << "\n";
    }

    Plan p(Layout::SoA, 2, 1, 4, 8, 1);
    Kernel k(p, Element::F32, Op::Scale, 2.0f, 0.0f);
    HostBuffer original(p);
    {
        auto loan = original.borrow_read();
        expect<std::logic_error>([&] { (void)original.take(); });
        expect<std::logic_error>([&] { original.write(0, 0, 1); });
        expect<std::logic_error>([&] { original.close(); });
    }
    auto moved = original.take();
    expect<std::logic_error>([&] { (void)original.read(0, 0); });
    expect<std::invalid_argument>([&] { execute_cpu(k, moved, moved); });
    expect<std::invalid_argument>([&] { (void)lower_opencl(k, {64, 1, 10, true}); });
    expect<std::invalid_argument>([&] { (void)lower_opencl(k, {64, 1024, 10, false}); });
    expect<std::invalid_argument>([&] { (void)lower_opencl(k, {32, 1024, 10, true}); });
    Kernel tiny(p, Element::F32, Op::Scale, std::numeric_limits<float>::denorm_min(), 0.0f);
    auto tinySource = lower_opencl(tiny, {64, 4096, 100, true}).device_source;
    check(tinySource.find("e-45f") != std::string::npos);
    moved.close();
    expect<std::logic_error>([&] { (void)moved.read(0, 0); });

    // Read-loan admission must serialize with ownership take() even when
    // the two calls originate on different carrier threads. No stale state_
    // pointer is ever dereferenced after ownership moves away.
    for (int iteration = 0; iteration < 300; ++iteration) {
        HostBuffer candidate(p);
        candidate.write(0, 0, 17.0f);
        std::atomic<bool> start{false};
        std::thread observer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                auto loan = candidate.borrow_read();
                check(loan.read(0, 0) == 17.0f);
            } catch (const std::logic_error&) {
                // Ownership transfer won; old wrapper must reject the loan.
            }
        });
        start.store(true, std::memory_order_release);
        bool transferred = false;
        try {
            auto successor = candidate.take();
            transferred = true;
            check(successor.layout().byte_length() == p.byte_length());
            check(successor.read(0, 0) == 17.0f);
        } catch (const std::logic_error&) {
            // Observer borrowed first; take() must fail until its loan drops.
        }
        observer.join();
        if (!transferred) {
            auto successor = candidate.take();
            check(successor.read(0, 0) == 17.0f);
        }
    }

    HostBuffer existing(p), replacement(p);
    expect<std::logic_error>([&] { existing = std::move(replacement); });
    check(existing.layout().rows() == p.rows());
    check(replacement.layout().rows() == p.rows());
    existing.close();
    existing = std::move(replacement);
    check(existing.layout().rows() == p.rows());
    expect<std::logic_error>([&] { (void)replacement.layout(); });
    std::cout << "native CPU/DOP and emitted GPU kernel IR tests passed\n";
}
