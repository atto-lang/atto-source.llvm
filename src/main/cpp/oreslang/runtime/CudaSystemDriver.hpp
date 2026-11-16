#pragma once

#include "oreslang/runtime/CudaCubinAot.hpp"

#include <cstdint>
#include <stdexcept>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
// NVIDIA CUDA device execution is unavailable on supported modern macOS.
#else
#include <dlfcn.h>
#endif

namespace oreslang::cuda_aot {

// Uses the NVIDIA *driver*, not the CUDA runtime compiler. No NVRTC, no PTX,
// no dynamic CUDA source compilation. A current context MUST already belong
// to the trusted tenant broker; this class never creates or owns a context.
class SystemDriver final : public Driver {
public:
    SystemDriver() {
#if defined(__APPLE__)
        throw std::runtime_error("CUDA native AOT unavailable on macOS");
#elif defined(_WIN32)
        library_ = LoadLibraryA("nvcuda.dll");
#else
        library_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library_) throw std::runtime_error("CUDA driver not available");
        try {
            init_ = symbol<Init>("cuInit");
            ctx_get_current_ = symbol<CtxGetCurrent>("cuCtxGetCurrent");
            ctx_get_device_ = symbol<CtxGetDevice>("cuCtxGetDevice");
            device_get_attribute_ = symbol<DeviceGetAttribute>("cuDeviceGetAttribute");
            module_load_ = symbol<ModuleLoad>("cuModuleLoadData");
            module_get_function_ = symbol<ModuleGetFunction>("cuModuleGetFunction");
            module_unload_ = symbol<ModuleUnload>("cuModuleUnload");
            launch_ = symbol<LaunchKernel>("cuLaunchKernel");
            event_create_ = symbol<EventCreate>("cuEventCreate");
            event_record_ = symbol<EventRecord>("cuEventRecord");
            event_query_ = symbol<EventQuery>("cuEventQuery");
            event_destroy_ = symbol<EventDestroy>("cuEventDestroy_v2");
            if (init_(0) != 0) throw std::runtime_error("CUDA initialization failed");
            void* context = nullptr;
            int device = -1;
            int major = -1, minor = -1;
            if (ctx_get_current_(&context) != 0 || !context
                    || ctx_get_device_(&device) != 0
                    || device_get_attribute_(&major, 75, device) != 0
                    || device_get_attribute_(&minor, 76, device) != 0
                    || major < 1 || minor < 0 || minor > 9)
                throw std::runtime_error("trusted CUDA context/SM unavailable");
            sm_ = static_cast<unsigned>(major * 10 + minor);
        } catch (...) {
            close_library();
            throw;
        }
    }
    SystemDriver(const SystemDriver&) = delete;
    SystemDriver& operator=(const SystemDriver&) = delete;
    ~SystemDriver() override { close_library(); }

    unsigned current_sm() override { return sm_; }
    int load_cubin(Handle* module, const unsigned char* image) override {
        return module_load_(module, image);
    }
    int get_function(Handle* fn, Handle module, const char* name) override {
        return module_get_function_(fn, module, name);
    }
    int unload(Handle module) override { return module_unload_(module); }
    int event_create(Handle* event) override {
        return event_create_(event, 2u); // CU_EVENT_DISABLE_TIMING
    }
    int launch(Handle fn, const Dimensions& d, void** args, Handle stream) override {
        return launch_(fn, d.grid_x, d.grid_y, d.grid_z,
            d.block_x, d.block_y, d.block_z, d.shared_bytes,
            stream, args, nullptr);
    }
    int event_record(Handle event, Handle stream) override {
        return event_record_(event, stream);
    }
    int event_query(Handle event) override { return event_query_(event); }
    int event_destroy(Handle event) override { return event_destroy_(event); }
private:
#if defined(_WIN32)
    HMODULE library_ = nullptr;
#elif defined(__APPLE__)
    void* library_ = nullptr;
#else
    void* library_ = nullptr;
#endif
    using Init = int (*)(unsigned);
    using CtxGetCurrent = int (*)(void**);
    using CtxGetDevice = int (*)(int*);
    using DeviceGetAttribute = int (*)(int*, int, int);
    using ModuleLoad = int (*)(void**, const void*);
    using ModuleGetFunction = int (*)(void**, void*, const char*);
    using ModuleUnload = int (*)(void*);
    using LaunchKernel = int (*)(void*, unsigned, unsigned, unsigned,
        unsigned, unsigned, unsigned, unsigned, void*, void**, void**);
    using EventCreate = int (*)(void**, unsigned);
    using EventRecord = int (*)(void*, void*);
    using EventQuery = int (*)(void*);
    using EventDestroy = int (*)(void*);
    Init init_ = nullptr;
    CtxGetCurrent ctx_get_current_ = nullptr;
    CtxGetDevice ctx_get_device_ = nullptr;
    DeviceGetAttribute device_get_attribute_ = nullptr;
    ModuleLoad module_load_ = nullptr;
    ModuleGetFunction module_get_function_ = nullptr;
    ModuleUnload module_unload_ = nullptr;
    LaunchKernel launch_ = nullptr;
    EventCreate event_create_ = nullptr;
    EventRecord event_record_ = nullptr;
    EventQuery event_query_ = nullptr;
    EventDestroy event_destroy_ = nullptr;
    unsigned sm_ = 0;

    template<class Fn> Fn symbol(const char* name) {
#if defined(_WIN32)
        const auto fn = GetProcAddress(library_, name);
#elif defined(__APPLE__)
        (void)name;
        void* fn = nullptr;
#else
        void* fn = dlsym(library_, name);
#endif
        if (!fn) throw std::runtime_error("required CUDA driver symbol unavailable");
        return reinterpret_cast<Fn>(fn);
    }
    void close_library() noexcept {
#if defined(_WIN32)
        if (library_) FreeLibrary(library_);
#elif defined(__APPLE__)
        // No CUDA dynamic library available.
#else
        if (library_) (void)dlclose(library_);
#endif
        library_ = nullptr;
    }
};
} // namespace oreslang::cuda_aot
