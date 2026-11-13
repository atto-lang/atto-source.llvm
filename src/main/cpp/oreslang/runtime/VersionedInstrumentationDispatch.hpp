#pragma once

// Host-only substrate for future LLVM ORC + Atto instrumentation. It neither
// compiles Atto source nor validates signed manifests; those are separate gates.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::runtime {

enum class InstrumentationMode { off, on, deep };
enum class PublishResult { accepted, unauthorized, unsupported, incompatible, stale_generation, invalid_variant };

struct InstrumentationIdentity {
    std::string function_id;
    std::string abi_sha256;
    std::string effects_sha256;
    std::string ownership_sha256;
    std::string continuation_sha256;

    static bool is_hex(const std::string& s, std::size_t required) noexcept {
        if (s.size() != required) return false;
        for (const auto c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    bool valid() const noexcept {
        return is_hex(function_id, 32) && is_hex(abi_sha256, 64) &&
               is_hex(effects_sha256, 64) && is_hex(ownership_sha256, 64) &&
               is_hex(continuation_sha256, 64);
    }
    bool operator==(const InstrumentationIdentity& o) const noexcept {
        return function_id == o.function_id && abi_sha256 == o.abi_sha256 &&
               effects_sha256 == o.effects_sha256 && ownership_sha256 == o.ownership_sha256 &&
               continuation_sha256 == o.continuation_sha256;
    }
};

using NativeInstrumentedFn = std::int64_t (*)(void*, std::int64_t) noexcept;

struct InstrumentedVersion {
    InstrumentationIdentity identity;
    std::uint64_t generation;
    NativeInstrumentedFn entry;
    // For an ORC version, this must own executable-memory lifetime. For a
    // context-bearing function, owner.get() must be the context pointer.
    std::shared_ptr<void> owner;
    void* context;
    bool aot_probe_variant;

    bool valid() const noexcept {
        return identity.valid() && generation > 0 && entry != nullptr &&
               (context == nullptr || (owner && owner.get() == context));
    }
};

// One slot per (tenant, function). The owner of a suspended continuation must
// additionally retain the shared_ptr<InstrumentedVersion> it started with.
// This class only proves in-flight synchronous native call retirement.
class VersionedInstrumentationDispatch final {
public:
    VersionedInstrumentationDispatch(std::shared_ptr<const InstrumentedVersion> baseline,
                                     bool aot_probes_supported, bool jit_codegen_supported,
                                     bool hard_realtime)
        : baseline_(std::move(baseline)), active_(baseline_),
          aot_probes_supported_(aot_probes_supported),
          jit_codegen_supported_(jit_codegen_supported), hard_realtime_(hard_realtime),
          highest_generation_(baseline_ ? baseline_->generation : 0) {
        if (!baseline_ || !baseline_->valid())
            throw std::invalid_argument("invalid immutable AOT baseline");
    }

    VersionedInstrumentationDispatch(const VersionedInstrumentationDispatch&) = delete;
    VersionedInstrumentationDispatch& operator=(const VersionedInstrumentationDispatch&) = delete;

    std::int64_t invoke(std::int64_t input) const noexcept {
        // Keep a strong lease until the invoked native function returns.
        const auto version = std::atomic_load_explicit(&active_, std::memory_order_acquire);
        return version->entry(version->context, input);
    }

    PublishResult publish(std::shared_ptr<const InstrumentedVersion> variant,
                          InstrumentationMode mode, bool tenant_authorized,
                          bool build_bound_signature_verified) {
        if (!tenant_authorized || !build_bound_signature_verified)
            return PublishResult::unauthorized;
        if (mode == InstrumentationMode::off) return PublishResult::unsupported;
        if (mode == InstrumentationMode::on && !aot_probes_supported_)
            return PublishResult::unsupported;
        if (mode == InstrumentationMode::deep && (!jit_codegen_supported_ || hard_realtime_))
            return PublishResult::unsupported;
        if (!baseline_ || !baseline_->valid() || !variant || !variant->valid())
            return PublishResult::invalid_variant;
        if (!(baseline_->identity == variant->identity))
            return PublishResult::incompatible;
        if (mode == InstrumentationMode::on && !variant->aot_probe_variant)
            return PublishResult::unsupported;
        std::lock_guard<std::mutex> guard(publish_mutex_);
        if (variant->generation <= highest_generation_)
            return PublishResult::stale_generation;
        highest_generation_ = variant->generation;
        // Only NEW invocations observe the new pointer; old leases remain live.
        std::atomic_store_explicit(&active_, std::move(variant), std::memory_order_release);
        return PublishResult::accepted;
    }

    // Turning tracing off is always allowed, including after revoking control
    // plane authorization. This is safe even while old calls execute.
    void disable() noexcept {
        std::lock_guard<std::mutex> guard(publish_mutex_);
        std::atomic_store_explicit(&active_, baseline_, std::memory_order_release);
    }

    std::uint64_t active_generation() const noexcept {
        const auto version = std::atomic_load_explicit(&active_, std::memory_order_acquire);
        return version ? version->generation : 0;
    }

private:
    const std::shared_ptr<const InstrumentedVersion> baseline_;
    mutable std::shared_ptr<const InstrumentedVersion> active_;
    const bool aot_probes_supported_;
    const bool jit_codegen_supported_;
    const bool hard_realtime_;
    mutable std::mutex publish_mutex_;
    std::uint64_t highest_generation_;
};

} // namespace oreslang::runtime
