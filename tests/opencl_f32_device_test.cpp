#include "oreslang/runtime/OpenClF32Device.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using namespace oreslang;
    const char* env = std::getenv("ORESLANG_OPENCL_DEVICE");
    const std::string preference = env ? env : "gpu";
    gpu::OpenClDeviceSelector selection;
    if (preference == "cpu") selection.required_type = CL_DEVICE_TYPE_CPU;
    else if (preference == "gpu") selection.required_type = CL_DEVICE_TYPE_GPU;
    else throw std::invalid_argument("explicit ORESLANG_OPENCL_DEVICE must be cpu or gpu");
    int executed = 0;
    for (auto layout : {dop::Layout::AoS, dop::Layout::SoA, dop::Layout::AoSoA}) {
        dop::Plan shape(layout, 7, 3, 4, 16, layout == dop::Layout::AoSoA ? 4u : 1u);
        dop::Kernel kernel(shape, dop::Element::F32, dop::Op::ScaleBias, 2.0f, 1.0f);
        auto artifact = dop::lower_opencl(kernel, {64, 1048576, 1024, true});
        std::vector<std::uint8_t> input(static_cast<std::size_t>(shape.byte_length()), 0);
        for (std::uint64_t row = 0; row < shape.rows(); ++row)
            for (std::uint32_t field = 0; field < shape.fields(); ++field) {
                const float value = static_cast<float>(row * shape.fields() + field) / 2.0f;
                const auto offset = static_cast<std::size_t>(shape.byte_offset(row, field));
                std::memcpy(input.data() + offset, &value, sizeof(value));
            }
        gpu::OpenClResult result;
        try {
            result = gpu::execute_opencl_f32(artifact, input, selection);
        } catch (const std::runtime_error& error) {
            const std::string msg = error.what();
            if (msg.find("requested OpenCL device type/name unavailable") != std::string::npos
                || msg.find("no OpenCL platform") != std::string::npos
                || msg.find("list platforms failed: -1001") != std::string::npos) {
                std::cout << "SKIP: explicit OpenCL " << preference << " device unavailable\n";
                return 77;
            }
            throw;
        }
        if (result.output.size() != shape.byte_length()
            || result.selected_device_type != selection.required_type)
            throw std::runtime_error("OpenCL device did not honor explicit selection");
        for (std::uint64_t row = 0; row < shape.rows(); ++row)
            for (std::uint32_t field = 0; field < shape.fields(); ++field) {
                const auto offset = static_cast<std::size_t>(shape.byte_offset(row, field));
                float actual = 0;
                std::memcpy(&actual, result.output.data() + offset, sizeof(actual));
                const float input_value = static_cast<float>(row * shape.fields() + field) / 2.0f;
                const float expected = input_value * 2.0f + 1.0f;
                if (std::abs(actual - expected) > 0.00001f)
                    throw std::runtime_error("OpenCL f32 DOP output differs from CPU reference");
            }
        ++executed;
        std::cout << "OpenCL " << preference << " device=" << result.device_name
                  << " layout=" << static_cast<int>(layout) << " PASS\n";
    }
    if (executed != 3) throw std::runtime_error("expected three layout runs");
}
