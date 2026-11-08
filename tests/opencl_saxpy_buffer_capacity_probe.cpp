#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"
#include "oreslang/runtime/AttoSaxpyProvider.hpp"
#include "oreslang/runtime/OpenClSystemApi.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

// Physical-driver negative probe: the SystemApi must refuse a SAXPY launch or
// transfer whose element count exceeds any bound buffer's driver-reported
// size, before anything is enqueued. The kernel's own `i < n` guard cannot
// protect against an undersized buffer.
namespace {

template <typename Call>
bool rejected(Call&& call) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        auto api = oreslang::opencl::SystemApi::try_create_gpu();
        if (!api) {
            std::cout << "SKIP:no-compatible-physical-opencl-gpu\n";
            return 77;
        }
        constexpr std::uint64_t n = 256;
        constexpr std::uint64_t bytes = n * sizeof(float);
        const auto artifact = oreslang::gpu_source_v1::compile_saxpy(
            "compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>, "
            "out: DeviceWrite<f32>, alpha: f32, n: u64) device { "
            "const i = gpu.global_id.x; if i < n { "
            "out[i] = alpha * x[i] + y[i]; } }");

        // Same artifact shape AttoSaxpyProvider builds for n rows.
        const oreslang::dop::OpenClArtifact device{
            artifact.kernel_name, artifact.device_source, bytes, n, 1,
            artifact.abi_version, artifact.required_address_bits,
            artifact.requires_f32, true};
        api->validate_artifact(device);
        auto program = api->prepare(device);
        auto x = api->allocate(bytes);
        auto y = api->allocate(bytes);
        auto small_out = api->allocate(16); // room for 4 floats, not 256
        std::vector<unsigned char> staging(bytes, 0);

        bool ok = true;
        if (!rejected([&] { (void)api->enqueue_saxpy(program, x, y, small_out, 2.0f, n); })) {
            std::cerr << "FAIL: kernel launch with undersized output buffer was admitted\n";
            ok = false;
        }
        if (!rejected([&] { (void)api->enqueue_saxpy(program, small_out, y, x, 2.0f, n); })) {
            std::cerr << "FAIL: kernel launch with undersized input buffer was admitted\n";
            ok = false;
        }
        if (!rejected([&] { (void)api->enqueue_upload(small_out, staging.data(), staging.size()); })) {
            std::cerr << "FAIL: upload larger than destination buffer was admitted\n";
            ok = false;
        }
        if (!rejected([&] { (void)api->enqueue_download(small_out, staging.data(), staging.size()); })) {
            std::cerr << "FAIL: download larger than source buffer was admitted\n";
            ok = false;
        }

        // Nothing was enqueued, so the buffers are quiescent and safe to free.
        api->release_buffer(small_out);
        api->release_buffer(y);
        api->release_buffer(x);
        api->release_program(program);
        if (!ok) return 1;
        std::cout << "ATTO_SAXPY_BUFFER_CAPACITY_REJECTED device="
                  << api->physical_device_name() << " rejected=4\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
