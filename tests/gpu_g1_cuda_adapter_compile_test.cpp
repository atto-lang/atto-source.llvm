#include "oreslang/backend/GpuG1CudaDriver.hpp"
#include <type_traits>
using namespace oreslang::gpu::g1;
static_assert(!std::is_copy_constructible<GpuG1CudaDriver>::value);
static_assert(std::is_same<GpuG1CudaDriver::Kernel,CUfunction>::value);
static_assert(std::is_same<GpuG1CudaDriver::Event,CUevent>::value);
static_assert(std::is_same<GpuG1CudaDriver::Buffer,CUdeviceptr>::value);
static_assert(!std::is_constructible<GpuG1CudaDriver, CUcontext, CUdevice, CUfunction>::value,
              "Raw CUDA kernel handles must never bypass verified PTX gate");
static_assert(std::is_constructible<GpuG1CudaDriver, CUcontext, CUdevice,
              GpuG1VerifiedCudaModule::KernelToken>::value,
              "Driver must admit a verified-module kernel token");
using Adapter = GpuG1EventProvider<GpuG1CudaDriver>;
static_assert(!std::is_copy_constructible<Adapter>::value);
int main() { return 0; }
