#pragma once

// This adapter is compiled ONLY when the CUDA Driver API SDK is present.
// It is not an AttoLang guest API; the trusted host owns the context, module
// and exact-head compiled G1 SAXPY CUfunction for the provider's lifetime.
#include "oreslang/backend/GpuG1EventProvider.hpp"
#include "oreslang/backend/GpuG1CudaModuleLoader.hpp"
#include <cuda.h>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::gpu::g1 {

class GpuG1CudaDriver final {
public:
    using Stream = CUstream;
    using Buffer = CUdeviceptr;
    using Event = CUevent;
    using Kernel = CUfunction;
    struct Limits { std::uint32_t maximum_threads_per_block; std::uint64_t maximum_grid_x; };

    // A raw caller-supplied CUfunction is NOT an authorization. The sole
    // constructor requires a verified PTX module token minted by the trusted
    // exact-head SHA-256 module gate.
    GpuG1CudaDriver(CUcontext context, CUdevice device,
                    GpuG1VerifiedCudaModule::KernelToken verified_kernel)
        : context_(context), device_(device), verified_kernel_(std::move(verified_kernel)),
          kernel_(verified_kernel_.kernel()) {
        if (!context_ || !kernel_ || verified_kernel_.owning_loader().context() != context_)
            fail("GPU-G1-CUDA-DRV-001: verified kernel/context mismatch");
        ContextGuard guard(context_);
        int max_threads=0, max_grid=0;
        checked(cuDeviceGetAttribute(&max_threads, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK,
                                     device_), "CUDA thread limit");
        checked(cuDeviceGetAttribute(&max_grid, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X,
                                     device_), "CUDA grid limit");
        if (max_threads<=0 || max_grid<=0)
            fail("GPU-G1-CUDA-DRV-001: device has no 1D launch capacity");
        limits_ = {static_cast<std::uint32_t>(max_threads), static_cast<std::uint64_t>(max_grid)};
    }
    GpuG1CudaDriver(const GpuG1CudaDriver&) = delete;
    GpuG1CudaDriver& operator=(const GpuG1CudaDriver&) = delete;
    Limits limits() const noexcept { return limits_; }
    bool valid_kernel(Kernel kernel) const noexcept { return kernel!=nullptr && kernel==kernel_; }
    Kernel vetted_kernel() const noexcept { return kernel_; }

    Stream create_stream() {
        std::lock_guard<std::mutex> lock(mutex_);
        ContextGuard guard(context_);
        Stream stream=nullptr;
        checked(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "CUDA stream create");
        try { streams_.insert(stream); }
        catch (...) { (void)cuStreamDestroy(stream); throw; }
        return stream;
    }
    void destroy_stream(Stream stream) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!streams_.count(stream)) fail("GPU-G1-CUDA-DRV-002: unowned stream");
        ContextGuard guard(context_);
        checked(cuStreamDestroy(stream), "CUDA stream destroy");
        streams_.erase(stream);
    }
    Buffer allocate(std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!bytes) fail("GPU-G1-CUDA-DRV-003: zero bytes");
        ContextGuard guard(context_);
        Buffer ptr=0;
        checked(cuMemAlloc(&ptr, bytes), "CUDA device allocate");
        try { allocations_.emplace(ptr,bytes); }
        catch (...) { (void)cuMemFree(ptr); throw; }
        return ptr;
    }
    void free(Buffer ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!allocations_.count(ptr)) fail("GPU-G1-CUDA-DRV-003: unowned allocation");
        ContextGuard guard(context_);
        checked(cuMemFree(ptr), "CUDA device free");
        allocations_.erase(ptr);
    }
    Buffer offset_ptr(Buffer base, std::uint64_t offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=allocations_.find(base);
        if (it==allocations_.end() || offset>it->second ||
            base>std::numeric_limits<Buffer>::max()-offset)
            fail("GPU-G1-CUDA-DRV-004: unsafe device pointer offset");
        return base+offset;
    }
    Event create_event() {
        std::lock_guard<std::mutex> lock(mutex_);
        ContextGuard guard(context_);
        Event event=nullptr;
        checked(cuEventCreate(&event, CU_EVENT_DISABLE_TIMING), "CUDA event create");
        try { events_.insert(event); }
        catch (...) { (void)cuEventDestroy(event); throw; }
        return event;
    }
    void record_event(Event event, Stream stream) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!events_.count(event) || !streams_.count(stream))
            fail("GPU-G1-CUDA-DRV-005: unowned event/stream");
        ContextGuard guard(context_);
        checked(cuEventRecord(event, stream), "CUDA event record");
    }
    HardwareEventState query_event(Event event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!events_.count(event)) return HardwareEventState::DeviceError;
        ContextGuard guard(context_);
        // NONBLOCKING: event only. NEVER use cuCtxSynchronize/cuStreamSynchronize
        // in a scheduler carrier or while waiting for cancellation.
        const auto result = cuEventQuery(event);
        if (result==CUDA_SUCCESS) return HardwareEventState::Completed;
        if (result==CUDA_ERROR_NOT_READY) return HardwareEventState::Pending;
        return HardwareEventState::DeviceError;
    }
    void destroy_event(Event event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!events_.count(event)) fail("GPU-G1-CUDA-DRV-005: unowned event");
        ContextGuard guard(context_);
        checked(cuEventDestroy(event), "CUDA event destroy");
        events_.erase(event);
    }
    void launch_saxpy(Kernel kernel, Stream stream, float scale, Buffer x, Buffer y,
                      Buffer out, std::uint64_t n, std::uint32_t groups,
                      std::uint32_t block_threads) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_kernel(kernel) || !streams_.count(stream) || !n || !groups ||
            !block_threads || block_threads>limits_.maximum_threads_per_block ||
            groups>limits_.maximum_grid_x)
            fail("GPU-G1-CUDA-DRV-006: unauthorized kernel/stream/geometry");
        ContextGuard guard(context_);
        // Driver API copies these parameter bytes during cuLaunchKernel.
        // ABI matches restricted G1 PTX: f32, device f32 arrays, u64 n.
        void* args[] = {&scale, &x, &y, &out, &n};
        checked(cuLaunchKernel(kernel,groups,1,1,block_threads,1,1,0,stream,args,nullptr),
                "CUDA launch saxpy");
    }

private:
    class ContextGuard {
    public:
        explicit ContextGuard(CUcontext ctx) {
            checked(cuCtxPushCurrent(ctx), "CUDA context push");
        }
        ~ContextGuard() noexcept {
            CUcontext popped=nullptr;
            if (cuCtxPopCurrent(&popped)!=CUDA_SUCCESS) std::terminate();
        }
        ContextGuard(const ContextGuard&)=delete;
        ContextGuard&operator=(const ContextGuard&)=delete;
    };
    [[noreturn]] static void fail(const char* text) { throw std::invalid_argument(text); }
    static void checked(CUresult result, const char* name) {
        if (result==CUDA_SUCCESS) return;
        const char* detail=nullptr;
        (void)cuGetErrorName(result,&detail);
        throw std::runtime_error(std::string("GPU-G1-CUDA-DRV-ERR: ")+name+" "+
                                 (detail?detail:"unknown CUDA error"));
    }
    CUcontext context_;
    CUdevice device_;
    GpuG1VerifiedCudaModule::KernelToken verified_kernel_;
    Kernel kernel_;
    Limits limits_{};
    mutable std::mutex mutex_;
    std::set<Stream> streams_;
    std::set<Event> events_;
    std::map<Buffer,std::size_t> allocations_;
};

} // namespace oreslang::gpu::g1
