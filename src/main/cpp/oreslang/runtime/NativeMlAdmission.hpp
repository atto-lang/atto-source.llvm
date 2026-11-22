#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang::ml {

// Staging boundary for issue #249. This is deliberately VOLATILE: never use
// it as a durable network or externally visible effect authorizer. A real
// adapter must atomically persist (tenant, resource, fence, version, effect).
class VolatileFencedSink final {
public:
    enum class Result {
        Committed, Invalid, ForeignScope, StaleFence, VersionConflict,
        VersionExhausted, Retired
    };
    struct Snapshot {
        std::uint64_t fence;
        std::uint64_t version;
        std::int64_t value;
    };

    VolatileFencedSink(std::string tenant, std::string resource, Snapshot initial)
        : tenant_(std::move(tenant)), resource_(std::move(resource)), state_(initial) {
        if (tenant_.empty() || resource_.empty() || initial.fence == 0)
            throw std::invalid_argument("fenced sink requires scoped nonzero initial fence");
    }
    VolatileFencedSink(const VolatileFencedSink&) = delete;
    VolatileFencedSink& operator=(const VolatileFencedSink&) = delete;

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    Result try_commit(std::string_view tenant, std::string_view resource,
                      std::uint64_t incoming_fence, std::uint64_t expected_version,
                      std::int64_t replacement) noexcept {
        if (tenant.empty() || resource.empty() || incoming_fence == 0)
            return Result::Invalid;
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return Result::Retired;
        if (tenant != tenant_ || resource != resource_) return Result::ForeignScope;
        if (incoming_fence < state_.fence) return Result::StaleFence;
        if (expected_version != state_.version) return Result::VersionConflict;
        if (state_.version == std::numeric_limits<std::uint64_t>::max())
            return Result::VersionExhausted;
        state_ = {incoming_fence, state_.version + 1, replacement};
        return Result::Committed;
    }

    void retire() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_ = true;
    }

private:
    const std::string tenant_;
    const std::string resource_;
    mutable std::mutex mutex_;
    Snapshot state_;
    bool retired_ = false;
};

// Issue #250: structural preflight ONLY. All syntactically/temporally valid
// claims are still denied until a maintained crypto verifier AND atomic
// redemption/authorization transaction are implemented. No boolean "signed"
// escape hatch or caller-provided proof can authorize effects here.
struct CapabilityClaim {
    std::uint64_t version;
    std::string_view tenant;
    std::string_view audience;
    std::string_view resource;
    std::uint64_t expires_at;
    std::uint64_t policy_revision;
    std::uint64_t resource_fence;
};
struct AdmissionContext {
    std::string_view tenant;
    std::string_view audience;
    std::string_view resource;
    std::uint64_t now;
    std::uint64_t policy_revision;
    std::uint64_t resource_fence;
};
enum class CapabilityDenial {
    UnsupportedVersion, MissingScope, ScopeMismatch, Expired,
    StalePolicy, StaleFence, SignatureAndRedemptionUnavailable
};
inline CapabilityDenial preflight_capability(const CapabilityClaim& claim,
                                             const AdmissionContext& ctx) noexcept {
    if (claim.version != 1) return CapabilityDenial::UnsupportedVersion;
    if (claim.tenant.empty() || claim.audience.empty() || claim.resource.empty()
            || ctx.tenant.empty() || ctx.audience.empty() || ctx.resource.empty()
            || claim.policy_revision == 0 || claim.resource_fence == 0)
        return CapabilityDenial::MissingScope;
    if (claim.tenant != ctx.tenant || claim.audience != ctx.audience
            || claim.resource != ctx.resource)
        return CapabilityDenial::ScopeMismatch;
    if (claim.expires_at <= ctx.now) return CapabilityDenial::Expired;
    if (claim.policy_revision != ctx.policy_revision)
        return CapabilityDenial::StalePolicy;
    if (claim.resource_fence != ctx.resource_fence)
        return CapabilityDenial::StaleFence;
    return CapabilityDenial::SignatureAndRedemptionUnavailable;
}

} // namespace oreslang::ml
