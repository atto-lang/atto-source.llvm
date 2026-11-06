#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace oreslang::cuda_aot {

// Native CUBIN ELF only. PTX, fatbin, LLVM bitcode and runtime-compilable
// OpenCL source never enter this execution profile.
struct Cubin {
    const unsigned char* bytes;
    std::size_t size;
    unsigned target_sm;
};

inline std::uint32_t u32le(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline unsigned cubin_sm(const Cubin& image) {
    // The GPU driver still validates the complete ELF. This bounded preflight
    // rules out PTX-as-text, fatbin containers and unexpected SM fallback.
    if (!image.bytes || image.size < 64 || image.size > 64u * 1024u * 1024u)
        throw std::invalid_argument("invalid or oversized AOT cubin");
    const auto* p = image.bytes;
    if (p[0] != 0x7f || p[1] != 'E' || p[2] != 'L' || p[3] != 'F'
            || p[4] != 2 || p[5] != 1 || p[6] != 1
            || p[16] != 2 || p[17] != 0     // ET_EXEC (finalized cubin)
            || p[18] != 190 || p[19] != 0  // EM_CUDA
            || u32le(p + 20) != 1
            || p[52] != 64 || p[53] != 0)  // ELF64 header size
        throw std::invalid_argument("CUDA AOT requires finalized ELF64 CUBIN, not PTX/fatbin");
    const auto flags = u32le(p + 48);
    // CUDA ELF flags have two published encodings. Strictly restrict the
    // initial no-finalization profile to known pre-Blackwell architectures.
    const unsigned legacy = flags & 0xffu;
    const unsigned newer = (flags >> 8) & 0xffu;
    auto supported = [](unsigned sm) {
        return sm == 70 || sm == 72 || sm == 75 || sm == 80
            || sm == 86 || sm == 87 || sm == 89 || sm == 90;
    };
    const unsigned sm = supported(newer) ? newer : legacy;
    if (!supported(sm) || (supported(newer) && supported(legacy) && newer != legacy)
            || image.target_sm != sm)
        throw std::invalid_argument("CUBIN SM missing, mismatched, or unsupported for strict AOT");
    return sm;
}

struct Dimensions {
    unsigned grid_x, grid_y, grid_z;
    unsigned block_x, block_y, block_z;
    unsigned shared_bytes;
};
struct Limits {
    std::uint64_t max_logical_threads;
    unsigned max_threads_per_block;
    unsigned max_shared_bytes;
    unsigned max_grid_x, max_grid_y, max_grid_z;
    unsigned max_block_x, max_block_y, max_block_z;
};
inline std::uint64_t checked_launch(const Dimensions& d, const Limits& l) {
    if (!d.grid_x || !d.grid_y || !d.grid_z || !d.block_x || !d.block_y || !d.block_z
            || d.grid_x > l.max_grid_x || d.grid_y > l.max_grid_y || d.grid_z > l.max_grid_z
            || d.block_x > l.max_block_x || d.block_y > l.max_block_y || d.block_z > l.max_block_z
            || d.shared_bytes > l.max_shared_bytes || !l.max_logical_threads)
        throw std::invalid_argument("invalid GPU launch geometry or memory limit");
    // Promote before multiplying; for six uint32 dimensions uint64 may
    // overflow. Check after each multiplication, never after a wrap.
    std::uint64_t count = 1;
    for (const auto n : {d.block_x, d.block_y, d.block_z}) {
        if (count > l.max_threads_per_block / static_cast<std::uint64_t>(n))
            throw std::invalid_argument("GPU block thread count exceeded");
        count *= n;
    }
    for (const auto n : {d.grid_x, d.grid_y, d.grid_z}) {
        if (count > l.max_logical_threads / static_cast<std::uint64_t>(n))
            throw std::invalid_argument("GPU launch thread budget exceeded");
        count *= n;
    }
    return count;
}

// Driver implementation is supplied by the trusted tenant/device broker.
// Interface deliberately contains no CUDA runtime compiler or linker entry.
// Handles are opaque, fixed-size, never managed by a language-level GC.
class Driver {
public:
    using Handle = void*;
    virtual ~Driver() = default;
    virtual unsigned current_sm() = 0;
    virtual int load_cubin(Handle*, const unsigned char*) = 0;
    virtual int get_function(Handle*, Handle, const char*) = 0;
    virtual int unload(Handle) = 0;
    virtual int event_create(Handle*) = 0;
    virtual int launch(Handle, const Dimensions&, void** args, Handle stream) = 0;
    virtual int event_record(Handle, Handle stream) = 0;
    // 0=complete; 600=CUDA_ERROR_NOT_READY; any other result is UNKNOWN,
    // not proof of quiescence. No resource release on unknown outcomes.
    virtual int event_query(Handle) = 0;
    virtual int event_destroy(Handle) = 0;
};

enum class Poll { Pending, Completed, Cancelled, Unknown };
class Kernel final {
public:
    Kernel(Driver& driver, const Cubin& image, const char* name, Limits limits)
        : driver_(driver), limits_(limits) {
        if (!name || !*name || !limits.max_threads_per_block || !limits.max_logical_threads)
            throw std::invalid_argument("missing GPU kernel or resource limits");
        if (cubin_sm(image) != driver_.current_sm())
            throw std::invalid_argument("CUBIN requires an exact device SM for strict AOT");
        const int load_status = driver_.load_cubin(&module_, image.bytes);
        if (load_status != 0 || !module_) {
            // Even a failed third-party driver may leave a module handle.
            // Do not silently lose its ownership on constructor failure.
            if (module_) {
                if (driver_.unload(module_) != 0) std::terminate();
                module_ = nullptr;
            }
            throw std::runtime_error("CUDA driver rejected precompiled cubin");
        }
        try {
            if (driver_.get_function(&function_, module_, name) != 0 || !function_)
                throw std::runtime_error("GPU kernel symbol is unavailable");
        } catch (...) {
            if (driver_.unload(module_) != 0) std::terminate();
            module_ = nullptr;
            throw;
        }
    }
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    Kernel(Kernel&&) = delete;
    Kernel& operator=(Kernel&&) = delete;
    ~Kernel() noexcept {
        // A dropped actor must not reclaim device code while a launch may
        // still execute. Runtime must retain this owner and complete drain.
        if (event_ || poisoned_) std::terminate();
        if (module_ && driver_.unload(module_) != 0) std::terminate();
    }
    void submit(Dimensions d, void** args, Driver::Handle stream) {
        if (!module_ || event_ || poisoned_ || !args || !stream)
            throw std::logic_error("GPU launch requires idle owner, args and stream");
        (void)checked_launch(d, limits_);
        if (driver_.event_create(&event_) != 0 || !event_) {
            // Event creation cannot authorize a dispatch. A non-null handle
            // after failed creation may be untrusted; retain it and poison.
            if (event_) poisoned_ = true;
            throw std::runtime_error("CUDA event admission failed");
        }
        if (driver_.launch(function_, d, args, stream) != 0) {
            // Driver failure need not prove that work wasn't enqueued.
            poisoned_ = true;
            throw std::runtime_error("CUDA launch outcome uncertain; owner retained");
        }
        if (driver_.event_record(event_, stream) != 0) {
            poisoned_ = true;
            throw std::runtime_error("CUDA fence outcome uncertain; owner retained");
        }
        cancel_requested_ = false;
    }
    void request_cancel() noexcept { if (event_) cancel_requested_ = true; }
    Poll poll() {
        if (poisoned_) return Poll::Unknown;
        if (!event_) throw std::logic_error("no GPU command in flight");
        const int status = driver_.event_query(event_);
        if (status == 600) return Poll::Pending;
        if (status != 0) return Poll::Unknown;
        if (driver_.event_destroy(event_) != 0) return Poll::Unknown;
        event_ = nullptr;
        const bool was_cancelled = cancel_requested_;
        cancel_requested_ = false;
        return was_cancelled ? Poll::Cancelled : Poll::Completed;
    }
    void close() {
        if (event_ || poisoned_) throw std::logic_error("device command not quiescent");
        if (!module_) return;
        if (driver_.unload(module_) != 0)
            throw std::runtime_error("device module unload not confirmed");
        module_ = nullptr;
    }
private:
    Driver& driver_;
    const Limits limits_;
    Driver::Handle module_ = nullptr;
    Driver::Handle function_ = nullptr;
    Driver::Handle event_ = nullptr;
    bool poisoned_ = false;
    bool cancel_requested_ = false;
};
} // namespace oreslang::cuda_aot
