#include "oreslang/runtime/AttoSaxpyProvider.hpp"
#include "oreslang/runtime/OpenClSystemApi.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace oreslang::opencl;
    if (argc != 2) {
        std::cerr << "usage: opencl-saxpy-physical-probe fixture.atto\n";
        return 2;
    }
    auto api = SystemApi::try_create_gpu();
    if (!api) {
        // Mandatory hardware lane must fail, not report a green test with a
        // skipped or CPU-backed OpenCL execution.
        std::cerr << "atto-saxpy-physical-not-executed:no-compatible-gpu\n";
        return 77;
    }
    try {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("cannot open AttoLang source fixture");
        const std::string source{std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>()};
        const std::vector<float> x{
            -0.25f, 0.5f, 1.0f, 1.25f, 2.0f, 3.5f, 4.0f, 5.25f};
        const std::vector<float> y{
            0.25f, -1.0f, 4.0f, 5.5f, 6.0f, 7.0f, -8.0f, 9.25f};
        const float alpha = 1.5f;
        const std::uint64_t bytes = x.size() * sizeof(float);
        SaxpyProvider provider(*api, "atto-gpu-probe",
                api->physical_device_name(), 1,
                {3 * bytes, 3 * bytes,
                 api->maximum_allocation_bytes(), 1024});
        auto operation = provider.prepare(source, x, y, alpha);
        try {
        auto state = operation->state();
        for (int i = 0; i < 10000; ++i) {
            state = provider.poll(*operation);
            if (state == SaxpyProvider::State::Succeeded) break;
            if (state == SaxpyProvider::State::Quarantined ||
                state == SaxpyProvider::State::Failed ||
                state == SaxpyProvider::State::ReleasePending)
                throw std::runtime_error("OpenCL SAXPY GPU operation failed or quarantined");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (state != SaxpyProvider::State::Succeeded)
            throw std::runtime_error("OpenCL SAXPY GPU completion timed out");
        const auto actual = operation->result();
        for (std::size_t i = 0; i < x.size(); ++i) {
            volatile float product = alpha * x[i];
            const float expected = product + y[i];
            std::uint32_t actual_bits = 0, expected_bits = 0;
            std::memcpy(&actual_bits, &actual[i], sizeof(actual_bits));
            std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
            if (actual_bits != expected_bits)
                throw std::runtime_error("physical OpenCL SAXPY f32 oracle mismatch");
        }
        if (provider.busy())
            throw std::runtime_error("SAXPY physical allocations not released");
        std::cout << "atto-saxpy-physical-gpu-passed:device="
                  << api->physical_device_name()
                  << " samples=" << x.size() << " bytes_per_buffer="
                  << bytes << " native_completion=verified\n";
        return 0;
        } catch (...) {
            // Out-of-band shutdown only, never an actor carrier. A failed
            // queue-quiescence proof must not free pinned DMA staging.
            if (provider.busy())
                provider.shutdown_after_quiescence(*operation);
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "atto-saxpy-physical-gpu-failed:" << e.what() << "\n";
        return 1;
    }
}
