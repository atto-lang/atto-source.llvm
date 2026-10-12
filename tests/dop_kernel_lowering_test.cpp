#include "oreslang/runtime/DopKernelIR.hpp"

#include <cmath>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>

static void check(bool condition) {
    if (!condition) throw std::runtime_error("typed DOP kernel test failed");
}
template<class Exception, class F>
static void expect(F fn) {
    try { fn(); } catch (const Exception&) { return; }
    throw std::runtime_error("expected DOP contract failure");
}

struct CommaPunct : std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
};

int main() {
    using namespace oreslang::dop;
    // A valid POD layout with undersized scalar slots must never become a
    // HostBuffer: F32 memcpy used to write past its allocation.
    for (auto bytes : {1u, 2u, 8u}) {
        Plan invalid_width(Layout::AoS, 1, 1, bytes, bytes, 1);
        expect<std::invalid_argument>([&] { HostBuffer buffer(invalid_width); });
    }
    // Emitted OpenCL must not inherit the process locale.
    const std::locale saved = std::locale();
    std::locale::global(std::locale(std::locale::classic(), new CommaPunct));
    const auto portable_constant = cl_float(1.25f);
    std::locale::global(saved);
    check(portable_constant.find("1.25") != std::string::npos);
    check(portable_constant.find(',') == std::string::npos);
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
    expect<std::invalid_argument>([&] {
        Kernel invalid_op(p, Element::F32, static_cast<Op>(255), 1.0f, 0.0f);
    });
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
    std::cout << "native CPU/DOP and emitted GPU kernel IR tests passed\n";
}
