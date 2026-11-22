#include "oreslang/backend/GpuG1CudaDriver.hpp"
#include <type_traits>
using namespace oreslang::gpu::g1;
static_assert(!std::is_copy_constructible<GpuG1CudaDriver>::value);
static_assert(std::is_same<GpuG1CudaDriver::Kernel,CUfunction>::value);
static_assert(std::is_same<GpuG1CudaDriver::Event,CUevent>::value);
static_assert(std::is_same<GpuG1CudaDriver::Buffer,CUdeviceptr>::value);
using Adapter = GpuG1EventProvider<GpuG1CudaDriver>;
static_assert(!std::is_copy_constructible<Adapter>::value);
int main() { return 0; }
