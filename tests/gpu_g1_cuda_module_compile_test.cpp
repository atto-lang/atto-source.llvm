#include "oreslang/backend/GpuG1CudaModuleLoader.hpp"
#include <type_traits>
using namespace oreslang::gpu::g1;
static_assert(!std::is_copy_constructible<GpuG1CudaModuleLoader>::value);
static_assert(!std::is_copy_constructible<GpuG1VerifiedCudaModule>::value);
static_assert(std::is_same<GpuG1CudaModuleLoader::Kernel,CUfunction>::value);
int main(){return 0;}
