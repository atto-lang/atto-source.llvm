#pragma once
#include "G1CudaProvider.hpp"
#include <cuda.h>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang::gpu::g2 {

// Real CUDA Driver API adapter; build only with CUDA SDK and libcuda.
// One CUcontext and nondefault stream. The caller must close every GPU
// operation before destroying the driver or its CUDA context.
class CudaDriver final : public Driver {
public:
    explicit CudaDriver(int ordinal) {
        require(cuInit(0), "cuInit");
        require(cuDriverGetVersion(&version_), "cuDriverGetVersion");
        require(cuDeviceGet(&device_, ordinal), "cuDeviceGet");
        require(cuDevicePrimaryCtxRetain(&context_, device_), "cuDevicePrimaryCtxRetain");
        try {
            current();
            require(cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
            char bus[64]{};
            require(cuDeviceGetPCIBusId(bus, static_cast<int>(sizeof(bus)), device_),
                    "cuDeviceGetPCIBusId");
            identity_ = std::string("cuda-pci:") + bus;
            limits_ = Limits{
                {attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X),
                 attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y),
                 attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z)},
                {attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X),
                 attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y),
                 attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z)},
                attribute(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK),
                attribute(CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK)};
        } catch (...) {
            if (stream_) (void)cuStreamDestroy(stream_);
            (void)cuDevicePrimaryCtxRelease(device_);
            context_ = nullptr;
            throw;
        }
    }
    CudaDriver(const CudaDriver&) = delete;
    CudaDriver& operator=(const CudaDriver&) = delete;
    ~CudaDriver() override {
        // Explicit operation quiescence and retirement are a prerequisite.
        if (context_) {
            (void)cuCtxSetCurrent(context_);
            if (stream_) (void)cuStreamDestroy(stream_);
            (void)cuDevicePrimaryCtxRelease(device_);
        }
    }
    int driver_version() const noexcept { return version_; }
    const std::string& device_identity() const override { return identity_; }
    const Limits& device_limits() const override { return limits_; }
    Handle load_ptx(std::string_view ptx) override {
        current();
        const std::string null_terminated(ptx);
        CUmodule module=nullptr;
        require(cuModuleLoadData(&module, null_terminated.c_str()), "cuModuleLoadData");
        return pack(module);
    }
    Handle kernel(Handle module, const char* entry) override {
        current(); CUfunction fn=nullptr;
        require(cuModuleGetFunction(&fn, unpack<CUmodule>(module), entry),
                "cuModuleGetFunction");
        return pack(fn);
    }
    Handle allocate(std::size_t bytes) override {
        current(); CUdeviceptr ptr=0;
        require(cuMemAlloc(&ptr, bytes), "cuMemAlloc");
        return static_cast<Handle>(ptr);
    }
    void upload(Handle dst, const float* src, std::size_t bytes) override {
        current();
        // Pageable staging can block at the CUDA host API boundary; this
        // dedicated *hardware-test* adapter is NOT an actor-carrier runtime.
        require(cuMemcpyHtoDAsync(static_cast<CUdeviceptr>(dst), src, bytes, stream_),
                "cuMemcpyHtoDAsync");
    }
    void launch(Handle fn, std::uint32_t grid, std::uint32_t group,
                float scale, Handle x, Handle y, Handle out, std::uint64_t n) override {
        current();
        CUdeviceptr dx=static_cast<CUdeviceptr>(x);
        CUdeviceptr dy=static_cast<CUdeviceptr>(y);
        CUdeviceptr dz=static_cast<CUdeviceptr>(out);
        void* arguments[] = {&scale, &dx, &dy, &dz, &n};
        require(cuLaunchKernel(unpack<CUfunction>(fn), grid, 1, 1,
                               group, 1, 1, 0, stream_, arguments, nullptr),
                "cuLaunchKernel");
    }
    void download(float* dst, Handle src, std::size_t bytes) override {
        current();
        require(cuMemcpyDtoHAsync(dst, static_cast<CUdeviceptr>(src), bytes, stream_),
                "cuMemcpyDtoHAsync");
    }
    Handle record_event() override {
        current(); CUevent event=nullptr;
        require(cuEventCreate(&event, CU_EVENT_DISABLE_TIMING), "cuEventCreate");
        try { require(cuEventRecord(event, stream_), "cuEventRecord"); }
        catch (...) { (void)cuEventDestroy(event); throw; }
        return pack(event);
    }
    EventResult event_result(Handle event) override {
        current(); const auto code=cuEventQuery(unpack<CUevent>(event));
        if (code == CUDA_SUCCESS) return EventResult::Complete;
        if (code == CUDA_ERROR_NOT_READY) return EventResult::Pending;
        return EventResult::Failed; // error never proves quiescence
    }
    bool stream_quiescent() override {
        current(); const auto code=cuStreamQuery(stream_);
        if (code == CUDA_SUCCESS) return true;
        if (code == CUDA_ERROR_NOT_READY) return false;
        throw std::runtime_error("GPU-G2-002: CUDA stream quiescence unverified");
    }
    void destroy_event(Handle event) override {
        current(); require(cuEventDestroy(unpack<CUevent>(event)), "cuEventDestroy");
    }
    void free_buffer(Handle buffer) override {
        current(); require(cuMemFree(static_cast<CUdeviceptr>(buffer)), "cuMemFree");
    }
    void unload(Handle module) override {
        current(); require(cuModuleUnload(unpack<CUmodule>(module)), "cuModuleUnload");
    }
private:
    static void require(CUresult result, const char* op) {
        if (result != CUDA_SUCCESS)
            throw std::runtime_error(std::string("GPU-G2-002: ") + op +
                                     " failed with CUDA code " + std::to_string(result));
    }
    void current() const { require(cuCtxSetCurrent(context_), "cuCtxSetCurrent"); }
    std::int64_t attribute(CUdevice_attribute attr) const {
        int result=0;
        require(cuDeviceGetAttribute(&result, attr, device_), "cuDeviceGetAttribute");
        return result;
    }
    template<class T> static Handle pack(T handle) {
        static_assert(sizeof(T) <= sizeof(Handle), "CUDA host handle too wide");
        return static_cast<Handle>(reinterpret_cast<std::uintptr_t>(handle));
    }
    template<class T> static T unpack(Handle handle) {
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(handle));
    }
    CUdevice device_{};
    CUcontext context_=nullptr;
    CUstream stream_=nullptr;
    int version_=0;
    std::string identity_;
    Limits limits_{{1,1,1},{1,1,1},1,0};
};
} // namespace oreslang::gpu::g2
