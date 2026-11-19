// Opt-in PHYSICAL CUDA kernel probe for typed G1 IR only, NOT an .atto guest.
// Requires CUDA Driver API and a real GPU; never marks host emulation as pass.
#include <cuda.h>
#include "oreslang/backend/GpuG1SaxpyLaunchAdmission.hpp"
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void ok(CUresult code, const char* step) {
    if (code == CUDA_SUCCESS) return;
    const char* name = nullptr;
    (void)cuGetErrorName(code, &name);
    throw std::runtime_error(std::string(step) + ": " + (name ? name : "unknown CUDA error"));
}
struct Context {
    CUcontext value = nullptr;
    explicit Context(CUdevice dev) { ok(cuCtxCreate(&value, 0, dev), "create context"); }
    ~Context() { if (value) (void)cuCtxDestroy(value); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};
struct Module {
    CUmodule value = nullptr;
    explicit Module(const std::string& ptx) {
        ok(cuModuleLoadData(&value, ptx.c_str()), "load PTX");
    }
    ~Module() { if (value) (void)cuModuleUnload(value); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};
struct Buffer {
    CUdeviceptr value = 0;
    explicit Buffer(std::size_t bytes) {
        if (!bytes || bytes > (1u << 24)) throw std::invalid_argument("invalid GPU budget");
        ok(cuMemAlloc(&value, bytes), "allocate");
    }
    ~Buffer() { if (value) (void)cuMemFree(value); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};
void case_on_device(CUfunction kernel, std::uint64_t n,
                    std::uint64_t device_max_threads, std::uint64_t device_max_groups) {
    constexpr unsigned int group = 256;
    const std::size_t count = static_cast<std::size_t>(n) + 2;
    const std::size_t bytes = count * sizeof(float);
    float scale = 1.75f;
    std::vector<float> x(count), y(count), output(count, -9000.0f);
    for (std::size_t i = 0; i < count; ++i) {
        x[i] = static_cast<float>(i % 101) / 16.0f;
        y[i] = static_cast<float>(i % 37) / 13.0f;
    }
    output[static_cast<std::size_t>(n)] = -543.5f;
    output[static_cast<std::size_t>(n + 1)] = 1234.25f;
    Buffer dx(bytes), dy(bytes), dout(bytes);
    ok(cuMemcpyHtoD(dx.value, x.data(), bytes), "upload x");
    ok(cuMemcpyHtoD(dy.value, y.data(), bytes), "upload y");
    ok(cuMemcpyHtoD(dout.value, output.data(), bytes), "upload out");
    // Bind the structural preflight to allocations actually returned by
    // the CUDA driver. Tenant/generation values remain TEST FIXTURES, not
    // authenticated provider capabilities or compiler borrow-checking proof.
    using namespace oreslang::gpu::g1;
    constexpr std::uint64_t fixture_tenant = 1;
    constexpr std::uint64_t fixture_device = 1;
    constexpr std::uint64_t fixture_generation = 1;
    const QueueLocation queue{fixture_tenant, fixture_device, fixture_generation, 1, 1};
    const LaunchLimits limits{device_max_threads, device_max_groups, 1U << 24};
    const DeviceView view_x{fixture_tenant, fixture_device, fixture_generation,
        static_cast<std::uint64_t>(dx.value), 1, count, 0, count, Loan::ReadOnly};
    const DeviceView view_y{fixture_tenant, fixture_device, fixture_generation,
        static_cast<std::uint64_t>(dy.value), 1, count, 0, count, Loan::ReadOnly};
    const DeviceView view_out{fixture_tenant, fixture_device, fixture_generation,
        static_cast<std::uint64_t>(dout.value), 1, count, 0, count, Loan::ExclusiveWrite};
    const auto plan = admit_saxpy_launch(view_x, view_y, view_out, queue,
                                         limits, n, group);
    if (plan.empty || plan.groups_x > std::numeric_limits<unsigned int>::max())
        throw std::runtime_error("GPU_G1_HW_INVALID_LAUNCH_PLAN");
    auto ax = dx.value, ay = dy.value, ao = dout.value;
    void* args[] = {&scale, &ax, &ay, &ao, &n};
    const auto grid = static_cast<unsigned int>(plan.groups_x);
    ok(cuLaunchKernel(kernel, grid, 1, 1, group, 1, 1, 0, nullptr, args, nullptr), "launch");
    // Test-only blocking wait. Production Atto Future must be nonblocking.
    ok(cuCtxSynchronize(), "device completion");
    ok(cuMemcpyDtoH(output.data(), dout.value, bytes), "download");
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
        const float want = scale * x[i] + y[i];
        if (!std::isfinite(output[i]) ||
            std::abs(output[i] - want) > 0.0001f * (1.0f + std::abs(want)))
            throw std::runtime_error("GPU_G1_HW_NUMERIC_MISMATCH");
    }
    if (output[static_cast<std::size_t>(n)] != -543.5f ||
        output[static_cast<std::size_t>(n + 1)] != 1234.25f)
        throw std::runtime_error("GPU_G1_HW_OOB_WRITE");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("required: compiled .ptx path");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("PTX unreadable");
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size <= 0 || size > (1u << 20))
            throw std::runtime_error("PTX exceeds bounded file budget");
        input.seekg(0, std::ios::beg);
        std::string ptx(static_cast<std::size_t>(size), '\0');
        input.read(ptx.data(), static_cast<std::streamsize>(size));
        if (!input || ptx.find("atto_g1_saxpy_f32") == std::string::npos)
            throw std::runtime_error("PTX untrusted or missing named G1 entry");
        ok(cuInit(0), "init physical CUDA driver");
        int ndevices = 0;
        ok(cuDeviceGetCount(&ndevices), "device count");
        if (ndevices < 1) throw std::runtime_error("no physical CUDA device; NO FALLBACK");
        CUdevice dev = 0;
        ok(cuDeviceGet(&dev, 0), "select device");
        int major = 0, minor = 0, driver = 0, max_threads = 0, max_grid_x = 0;
        char name[256] = {};
        ok(cuDeviceComputeCapability(&major, &minor, dev), "GPU architecture");
        ok(cuDriverGetVersion(&driver), "driver version");
        ok(cuDeviceGetName(name, sizeof(name), dev), "device name");
        ok(cuDeviceGetAttribute(&max_threads,
            CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, dev), "thread limit");
        ok(cuDeviceGetAttribute(&max_grid_x,
            CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, dev), "grid limit");
        if (major < 5 || max_threads < 256 || max_grid_x < 257)
            throw std::runtime_error("device cannot execute G1 geometry");
        Context context(dev);
        Module module(ptx);
        CUfunction kernel = nullptr;
        ok(cuModuleGetFunction(&kernel, module.value, "atto_g1_saxpy_f32"), "resolve entry");
        for (std::uint64_t n : {1ULL, 255ULL, 256ULL, 257ULL, 1025ULL, 65537ULL})
            case_on_device(kernel, n, static_cast<std::uint64_t>(max_threads),
                           static_cast<std::uint64_t>(max_grid_x));
        std::cout << "GPU_G1_PHYSICAL_PASS 6/6 driver=" << driver << " device="
                  << name << " sm=" << major << '.' << minor
                  << " evidence=PTX_TYPED_IR_ONLY_NOT_ATTO_GUEST\n";
    } catch (const std::exception& error) {
        std::cerr << "GPU_G1_PHYSICAL_FAIL " << error.what() << '\n';
        return 1;
    }
}
