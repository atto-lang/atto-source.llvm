#include "oreslang/runtime/DopKernelIR.hpp"
#include <stdexcept>
#include <utility>

namespace {
void check(bool valid) {
    if (!valid) throw std::runtime_error("CPU region slice invariant failed");
}
template<class F> void must_reject(F fn) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid region slice was accepted");
}
}

int main() {
    using namespace oreslang::dop;
    for (auto kind : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        Plan layout(kind, 7, 3, 4, 16, kind == Layout::AoSoA ? 4 : 1);
        Kernel kernel(layout, Element::F32, Op::ScaleBias, 2.0f, 1.0f);
        HostBuffer input(layout), output(layout), untrusted(layout);
        for (std::uint64_t row = 0; row < layout.rows(); ++row)
            for (std::uint32_t field = 0; field < layout.fields(); ++field)
                input.write(row, field, static_cast<float>(10 * row + field));
        auto in_root = input.region_root();
        auto out_root = output.region_root();
        const RegionAccess read(in_root, 1, 2, 3, RegionMode::Read);
        const RegionAccess write(out_root, 1, 2, 3, RegionMode::Write);
        execute_cpu_region(kernel, input, output, read, write);
        for (std::uint64_t row = 0; row < layout.rows(); ++row)
            for (std::uint32_t field = 0; field < layout.fields(); ++field) {
                const float expected = row >= 2 && row < 5 && field == 1
                    ? static_cast<float>(2 * (10 * row + 1) + 1) : 0.0f;
                check(output.read(row, field) == expected);
            }
        must_reject([&] { execute_cpu_region(kernel, input, output,
            RegionAccess(RegionRoot::trusted_create(7, 3), 1, 2, 3, RegionMode::Read), write); });
        must_reject([&] { execute_cpu_region(kernel, input, output,
            RegionAccess(untrusted.region_root(), 1, 2, 3, RegionMode::Read), write); });
        must_reject([&] { execute_cpu_region(kernel, input, output,
            RegionAccess(in_root, 1, 2, 3, RegionMode::Write), write); });
        must_reject([&] { execute_cpu_region(kernel, input, output, read,
            RegionAccess(out_root, 2, 2, 3, RegionMode::Write)); });
        must_reject([&] { execute_cpu_region(kernel, input, output, read,
            RegionAccess(out_root, 1, 3, 2, RegionMode::Write)); });
        must_reject([&] { execute_cpu_region(kernel, input, input, read,
            RegionAccess(in_root, 1, 2, 3, RegionMode::Write)); });
        {
            auto reserved = output.borrow_read();
            must_reject([&] { execute_cpu_region(kernel, input, output, read, write); });
            check(reserved.read(0, 0) == 0.0f);
        }
        // Failed write admission must release its earlier input read loan.
        auto input_successor = input.take();
        check(input_successor.region_root().same_root(in_root));
        execute_cpu_region(kernel, input_successor, output, read, write);
        output.close();
        check(!out_root.is_live_owned());
        must_reject([&] { execute_cpu_region(kernel, input_successor, output, read, write); });
        input_successor.close();
        untrusted.close();
    }
}
