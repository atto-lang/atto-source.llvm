#pragma once

#include "oreslang/runtime/TrustedOutboundTokenBucket.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted host-side quota substrate, not yet connected to OresVM socket/HTTP
 * broker, compiler-lowered guest networking, or supervisor route admission.
 * NOT a secure in-process UNTRUSTED native execution boundary.
 *
 * This prototype uses only fixed-size state. No guest memory or tracing GC.
 * Stable authenticated lineage/tenant aggregate budget, DNS permissions,
 * socket pool accounting, and deadline/cancellation bridges are still TODO.
 */
class TrustedUntrustedNetworkQuota final {
public:
    struct Policy {
        std::uint64_t http_starts_per_second = 10;
        std::uint64_t http_burst = 2;
        std::uint32_t max_http_in_flight = 10;
        std::uint32_t max_tcp_connections = 10;
        std::uint64_t tcp_starts_per_second = 10;
        std::uint64_t tcp_burst = 2;
        bool valid() const noexcept {
            return http_starts_per_second > 0
                && http_starts_per_second <= TrustedOutboundTokenBucket::kMaxRateOrBurst
                && http_burst > 0
                && http_burst <= TrustedOutboundTokenBucket::kMaxRateOrBurst
                && max_http_in_flight > 0 && max_http_in_flight <= 1000000
                && max_tcp_connections > 0 && max_tcp_connections <= 1000000
                && tcp_starts_per_second > 0
                && tcp_starts_per_second <= TrustedOutboundTokenBucket::kMaxRateOrBurst
                && tcp_burst > 0
                && tcp_burst <= TrustedOutboundTokenBucket::kMaxRateOrBurst;
        }
    };

    enum class Status {
        admitted, rate_limited, too_many_in_flight, too_many_connections, closed, invalid_policy
    };

    enum class Kind : std::uint8_t { http, tcp };

    class Lease final {
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : owner_(other.owner_), kind_(other.kind_),
              active_(other.active_.exchange(false)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = other.owner_;
                kind_ = other.kind_;
                active_.store(other.active_.exchange(false));
            }
            return *this;
        }
        ~Lease() noexcept { release(); }
        void release() noexcept {
            if (active_.exchange(false)) owner_->release(kind_);
        }
        explicit operator bool() const noexcept { return active_.load(); }
    private:
        friend class TrustedUntrustedNetworkQuota;
        Lease(TrustedUntrustedNetworkQuota* owner, Kind kind) noexcept
            : owner_(owner), kind_(kind), active_(true) {}
        TrustedUntrustedNetworkQuota* owner_ = nullptr;
        Kind kind_ = Kind::http;
        std::atomic<bool> active_{false};
    };

    struct Admission {
        Status status;
        Lease lease;
    };

    TrustedUntrustedNetworkQuota(Policy policy, std::uint64_t initial_host_nanos) noexcept
        : policy_(policy),
          http_start_(policy.http_starts_per_second, policy.http_burst, initial_host_nanos),
          tcp_start_(policy.tcp_starts_per_second, policy.tcp_burst, initial_host_nanos) {}
    ~TrustedUntrustedNetworkQuota() noexcept {
        // A broker must drain/close all host operations before destroying
        // the quota owner; a live RAII lease may never outlive this object.
        std::lock_guard<std::mutex> hold(lock_);
        if (http_in_flight_ != 0 || tcp_connections_ != 0) std::abort();
    }

    TrustedUntrustedNetworkQuota(const TrustedUntrustedNetworkQuota&) = delete;
    TrustedUntrustedNetworkQuota& operator=(const TrustedUntrustedNetworkQuota&) = delete;

    Admission try_start_http(std::uint64_t trusted_nanos) noexcept {
        if (!policy_.valid() || !http_start_.valid_policy())
            return {Status::invalid_policy, Lease{}};
        std::lock_guard<std::mutex> hold(lock_);
        if (closed_) return {Status::closed, Lease{}};
        if (http_in_flight_ == policy_.max_http_in_flight)
            return {Status::too_many_in_flight, Lease{}};
        if (!http_start_.try_consume(1, trusted_nanos))
            return {Status::rate_limited, Lease{}};
        ++http_in_flight_;
        return {Status::admitted, Lease(this, Kind::http)};
    }

    Admission try_open_tcp(std::uint64_t trusted_nanos) noexcept {
        if (!policy_.valid() || !http_start_.valid_policy() || !tcp_start_.valid_policy())
            return {Status::invalid_policy, Lease{}};
        std::lock_guard<std::mutex> hold(lock_);
        if (closed_) return {Status::closed, Lease{}};
        if (tcp_connections_ == policy_.max_tcp_connections)
            return {Status::too_many_connections, Lease{}};
        // Charge the dial attempt independently from the live socket slot:
        // repeated DNS/connect failures still consume start-rate tokens.
        if (!tcp_start_.try_consume(1, trusted_nanos))
            return {Status::rate_limited, Lease{}};
        ++tcp_connections_;
        return {Status::admitted, Lease(this, Kind::tcp)};
    }

    std::uint32_t http_in_flight() const noexcept {
        std::lock_guard<std::mutex> hold(lock_);
        return http_in_flight_;
    }
    std::uint32_t tcp_connections() const noexcept {
        std::lock_guard<std::mutex> hold(lock_);
        return tcp_connections_;
    }
    void close() noexcept {
        std::lock_guard<std::mutex> hold(lock_);
        closed_ = true;
    }

private:
    void release(Kind kind) noexcept {
        std::lock_guard<std::mutex> hold(lock_);
        auto& count = kind == Kind::http ? http_in_flight_ : tcp_connections_;
        if (count == 0) std::abort(); // trusted-side use-after-release invariant
        --count;
    }

    const Policy policy_;
    TrustedOutboundTokenBucket http_start_;
    TrustedOutboundTokenBucket tcp_start_;
    mutable std::mutex lock_;
    std::uint32_t http_in_flight_ = 0;
    std::uint32_t tcp_connections_ = 0;
    bool closed_ = false;
};

} // namespace oreslang::runtime
