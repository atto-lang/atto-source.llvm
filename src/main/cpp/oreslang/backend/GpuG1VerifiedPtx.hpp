#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang::gpu::g1 {

// A verified byte-to-pinned-hash gate, NOT independent provenance or attestation.
// The caller MUST obtain these values from an authenticated exact-head build
// manifest, outside the guest/receipt's control. This is host-provider-only.
struct PinnedG1Ptx {
    std::string head_sha1;
    std::string tree_sha1;
    std::string fixture_blob_sha1;
    std::string compiler_sha256;
    std::string ptx_sha256;
    std::string entrypoint;
    std::uint32_t abi_version;
};

class GpuG1PtxGate final {
public:
    static constexpr std::size_t maximum_ptx_bytes = 1024U * 1024U;
    static constexpr std::string_view sole_entry = "atto_g1_saxpy_f32";

    static void verify(std::string_view ptx, const PinnedG1Ptx& pins) {
        valid_hex(pins.head_sha1, 40, "head");
        valid_hex(pins.tree_sha1, 40, "tree");
        valid_hex(pins.fixture_blob_sha1, 40, "fixture");
        valid_hex(pins.compiler_sha256, 64, "compiler");
        valid_hex(pins.ptx_sha256, 64, "ptx");
        if (pins.abi_version != 1 || pins.entrypoint != sole_entry)
            deny("GPU-G1-MODULE-001: unsupported kernel ABI or entrypoint");
        // CUDA's cuModuleLoadData is NUL-terminated for text PTX. Never allow
        // hidden trailing text after an embedded NUL or an oversized artifact.
        if (ptx.empty() || ptx.size() > maximum_ptx_bytes ||
            ptx.find('\0') != std::string_view::npos)
            deny("GPU-G1-MODULE-002: invalid PTX byte envelope");
        std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
        unsigned int length = 0;
        if (EVP_Digest(ptx.data(), ptx.size(), out.data(), &length, EVP_sha256(), nullptr) != 1 ||
            length != 32)
            deny("GPU-G1-MODULE-003: SHA-256 unavailable");
        std::array<unsigned char, 32> expected{};
        for (std::size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<unsigned char>((hex(pins.ptx_sha256[2*i]) << 4) |
                                                     hex(pins.ptx_sha256[2*i+1]));
        if (CRYPTO_memcmp(out.data(), expected.data(), expected.size()) != 0)
            deny("GPU-G1-MODULE-004: compiled PTX SHA-256 differs from independently pinned artifact");
    }
private:
    [[noreturn]] static void deny(const char* s) { throw std::invalid_argument(s); }
    static int hex(char c) noexcept {
        if (c >= '0' && c <= '9') return c-'0';
        if (c >= 'a' && c <= 'f') return c-'a'+10;
        return -1;
    }
    static void valid_hex(const std::string& s, std::size_t size, const char* name) {
        if (s.size() != size) deny("GPU-G1-MODULE-001: malformed independently pinned identity");
        for (char c : s) if (hex(c) < 0)
            deny("GPU-G1-MODULE-001: malformed independently pinned identity");
        (void)name;
    }
};

// Loader is a *trusted host* CUDA wrapper. Its load_ptx accepts NUL-terminated
// authenticated bytes, resolve_kernel returns only the named approved entry,
// and unload_module releases the module. Never pass untrusted guest closures.
// The kernel token intentionally holds the state alive while a CUDA driver is
// using that function. No automatic unload on destruction: unclean shutdown
// LEAKS rather than unloading code potentially still running on the device.
template<class Loader>
class GpuG1VerifiedPtxModule final {
public:
    using Module = typename Loader::Module;
    using Kernel = typename Loader::Kernel;
    struct State {
        Module module;
        Kernel kernel;
        bool loaded;
    };
    class KernelToken final {
        friend class GpuG1VerifiedPtxModule;
        std::shared_ptr<State> state_;
        explicit KernelToken(std::shared_ptr<State> state):state_(std::move(state)){}
    public:
        Kernel kernel() const {
            if (!state_ || !state_->loaded)
                throw std::invalid_argument("GPU-G1-MODULE-005: stale kernel token");
            return state_->kernel;
        }
    };

    GpuG1VerifiedPtxModule(Loader& loader, std::string_view ptx, PinnedG1Ptx pins)
        : loader_(loader), pins_(std::move(pins)) {
        GpuG1PtxGate::verify(ptx, pins_); // BEFORE any CUDA/module operation
        std::string terminated(ptx);
        terminated.push_back('\0');
        Module m = loader_.load_ptx(terminated);
        try {
            Kernel k = loader_.resolve_kernel(m, GpuG1PtxGate::sole_entry);
            state_ = std::make_shared<State>(State{m,k,true});
        } catch (...) {
            try { loader_.unload_module(m); }
            catch (...) { /* fail-closed: caller must quarantine CUDA context */ }
            throw;
        }
    }
    GpuG1VerifiedPtxModule(const GpuG1VerifiedPtxModule&) = delete;
    GpuG1VerifiedPtxModule& operator=(const GpuG1VerifiedPtxModule&) = delete;
    KernelToken token() const {
        if (!state_ || !state_->loaded)
            throw std::invalid_argument("GPU-G1-MODULE-005: module not loaded");
        return KernelToken{state_};
    }
    const PinnedG1Ptx& pins() const noexcept { return pins_; }
    template<class EventProvider>
    void close_after_quiescence(const EventProvider& provider) {
        if (!state_ || !state_->loaded)
            throw std::invalid_argument("GPU-G1-MODULE-006: duplicate module unload");
        provider.require_quiescent_shutdown();
        // An active trusted driver token prevents premature module unload,
        // even when no event is currently in flight.
        if (state_.use_count() != 1)
            throw std::invalid_argument("GPU-G1-MODULE-007: module held by a driver");
        loader_.unload_module(state_->module);
        state_->loaded = false;
    }
private:
    Loader& loader_;
    PinnedG1Ptx pins_;
    std::shared_ptr<State> state_;
};

} // namespace oreslang::gpu::g1
