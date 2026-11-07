#pragma once

#include "oreslang/backend/GpuG1VerifiedPtx.hpp"
#include <cuda.h>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <exception>

namespace oreslang::gpu::g1 {

// Trusted CUDA Driver API loader. The exact source-tree/compiler/PTX pins
// come from a separately authenticated runner manifest, never the guest.
// Kernel handles are exposed only through GpuG1VerifiedPtxModule::KernelToken.
class GpuG1CudaModuleLoader final {
public:
    using Module = CUmodule;
    using Kernel = CUfunction;
    explicit GpuG1CudaModuleLoader(CUcontext context) : context_(context) {
        if (!context_) throw std::invalid_argument("GPU-G1-MODULE-CUDA-001: missing context");
    }
    GpuG1CudaModuleLoader(const GpuG1CudaModuleLoader&) = delete;
    GpuG1CudaModuleLoader& operator=(const GpuG1CudaModuleLoader&) = delete;
    CUcontext context() const noexcept { return context_; }

    Module load_ptx(const std::string& terminated) {
        if (terminated.empty() || terminated.back() != '\0' ||
            terminated.find('\0') != terminated.size()-1 ||
            terminated.size() > GpuG1PtxGate::maximum_ptx_bytes + 1)
            throw std::invalid_argument("GPU-G1-MODULE-CUDA-002: noncanonical PTX envelope");
        std::lock_guard<std::mutex> lock(mu_);
        CurrentContext scope(context_);
        Module module = nullptr;
        checked(cuModuleLoadData(&module, terminated.data()), "load vetted PTX");
        if (!module) throw std::runtime_error("GPU-G1-MODULE-CUDA-003: CUDA returned no module");
        try { modules_.insert(module); }
        catch (...) { (void)cuModuleUnload(module); throw; }
        return module;
    }
    Kernel resolve_kernel(Module module, std::string_view name) {
        if (name != GpuG1PtxGate::sole_entry)
            throw std::invalid_argument("GPU-G1-MODULE-CUDA-004: forbidden entrypoint");
        std::lock_guard<std::mutex> lock(mu_);
        if (!modules_.count(module))
            throw std::invalid_argument("GPU-G1-MODULE-CUDA-005: foreign module handle");
        CurrentContext scope(context_);
        Kernel kernel = nullptr;
        constexpr char entry[] = "atto_g1_saxpy_f32";
        checked(cuModuleGetFunction(&kernel, module, entry), "resolve vetted SAXPY entrypoint");
        if (!kernel) throw std::runtime_error("GPU-G1-MODULE-CUDA-006: missing kernel handle");
        return kernel;
    }
    void unload_module(Module module) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!modules_.count(module))
            throw std::invalid_argument("GPU-G1-MODULE-CUDA-005: stale or foreign module");
        CurrentContext scope(context_);
        checked(cuModuleUnload(module), "unload quiescent module");
        modules_.erase(module);
    }
private:
    class CurrentContext {
    public:
        explicit CurrentContext(CUcontext c) { checked(cuCtxPushCurrent(c), "push context"); }
        ~CurrentContext() noexcept {
            CUcontext old = nullptr;
            if (cuCtxPopCurrent(&old) != CUDA_SUCCESS) std::terminate();
        }
        CurrentContext(const CurrentContext&) = delete;
        CurrentContext& operator=(const CurrentContext&) = delete;
    };
    static void checked(CUresult r, const char* operation) {
        if (r == CUDA_SUCCESS) return;
        const char* error = nullptr;
        (void)cuGetErrorName(r, &error);
        throw std::runtime_error(std::string("GPU-G1-MODULE-CUDA-ERR: ")+operation+" "+
                                 (error ? error : "unknown"));
    }
    CUcontext context_;
    std::mutex mu_;
    std::set<Module> modules_;
};

using GpuG1VerifiedCudaModule = GpuG1VerifiedPtxModule<GpuG1CudaModuleLoader>;

} // namespace oreslang::gpu::g1
