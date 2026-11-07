#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"
#include "oreslang/runtime/OpenClSystemApi.hpp"
#include "oreslang/runtime/SaxpyOpenClProvider.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::uint32_t bits(float input) {
    std::uint32_t result = 0;
    static_assert(sizeof(result) == sizeof(input), "f32 ABI required");
    std::memcpy(&result, &input, sizeof(result));
    return result;
}
bool terminal(oreslang::opencl::SaxpyProvider::State state) {
    using S = oreslang::opencl::SaxpyProvider::State;
    return state == S::Succeeded || state == S::Cancelled ||
           state == S::Failed || state == S::Quarantined ||
           state == S::ReleaseFailed;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: opencl-saxpy-gpu-probe <source.atto>\n";
            return 2;
        }
        std::ifstream source(argv[1], std::ios::binary);
        if (!source) throw std::runtime_error("SAXPY probe source unreadable");
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        const auto kernel = oreslang::gpu_source_v1::compile_saxpy(text);

        // The actual SystemApi selects only CL_DEVICE_TYPE_GPU; there is no
        // software OpenCL CPU device or scalar fallback for this probe.
        auto api = oreslang::opencl::SystemApi::try_create_gpu();
        if (!api) {
            std::cout << "SKIP:no-compatible-physical-opencl-gpu\n";
            return 77; // CTest must display SKIP, not a hardware PASS.
        }
        constexpr std::uint64_t n = 256;
        constexpr std::uint64_t bytes = n * sizeof(float);
        if (api->authenticated_max_allocation() < bytes)
            throw std::runtime_error("physical GPU cannot hold an f32 source buffer");

        oreslang::opencl::SaxpyProvider provider(
            *api,
            {"atto-saxpy-physical", api->authenticated_device_name(), 1, 1},
            {bytes * 3, bytes * 3, n});
        std::vector<float> x(n), y(n);
        const float alpha = 1.5f;
        for (std::uint64_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(i) * 0.25f + 0.5f;
            y[i] = static_cast<float>(i) * 0.0625f - 1.0f;
        }

        auto operation = provider.begin(kernel, x, y, alpha);
        auto state = operation->state();
        for (int attempt = 0; attempt < 20000 && !terminal(state); ++attempt) {
            state = provider.poll(*operation);
            if (!terminal(state))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!terminal(state)) {
            // Timeout is cancellation, not quiescence or an excuse to free
            // native handles. Keep the provider and operation alive while
            // attempting an authenticated event drain.
            provider.request_cancel(*operation);
            for (int attempt = 0; attempt < 20000 && !terminal(state); ++attempt) {
                state = provider.poll(*operation);
                if (!terminal(state))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (state != oreslang::opencl::SaxpyProvider::State::Succeeded) {
            std::cerr << "SAXPY physical GPU FAILED state="
                      << static_cast<int>(state)
                      << " device=" << api->authenticated_device_name()
                      << "\n";
            return 1;
        }

        const auto output = operation->result();
        if (output.size() != n)
            throw std::runtime_error("physical GPU returned wrong output length");
        for (std::uint64_t i = 0; i < n; ++i) {
            volatile float product = alpha * x[i];
            const float expected = product + y[i];
            if (bits(output[i]) != bits(expected)) {
                std::cerr << "physical GPU SAXPY oracle mismatch at " << i
                          << " actual=" << output[i] << " expected=" << expected
                          << "\n";
                return 1;
            }
        }
        if (!provider.closed_for_release() ||
                provider.usage().resident_bytes != 0 ||
                provider.usage().host_staging_bytes != 0 ||
                provider.usage().queue_slots != 0)
            throw std::runtime_error("GPU resource quota retained after device completion");

        std::cout << "ATTO_SAXPY_PHYSICAL_GPU_PASSED"
                  << " device=" << api->authenticated_device_name()
                  << " elements=" << n
                  << " queue=1 generation=" << operation->allocation_generation()
                  << " verified_outputs=" << output.size()
                  << " event_stages=4"
                  << " released_buffers=3"
                  << " output_checksum_bits=" << bits(output.back())
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SAXPY physical GPU probe error: " << error.what() << "\n";
        return 1;
    }
}
