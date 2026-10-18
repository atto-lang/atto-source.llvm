#pragma once

#include "oreslang/runtime/TrustedUntrustedNetworkQuota.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace oreslang::runtime {

/**
 * HOST-ONLY atomic three-scope egress gate: child lineage, supervisor, tenant.
 *
 * No dynamic allocation or tracing GC inside admission. A trusted broker must
 * bind actual verified actors to stable Scope objects and own their lifetime.
 * This is NOT wired to guest network lowering or an isolation boundary.
 * NEVER hand a Scope, Lease, timestamp or policy pointer to guest code.
 */
class TrustedHierarchicalNetworkGate final {
public:
    using Quota = TrustedUntrustedNetworkQuota;
    using Status = Quota::Status;

    class Scope final {
    public:
        Scope(TrustedHierarchicalNetworkGate& gate, Quota::Policy policy,
              std::uint64_t initial_host_nanos) noexcept
            : gate_(&gate), quota_(policy, initial_host_nanos) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;
        void close() noexcept {
            std::lock_guard<std::mutex> guard(gate_->lock_);
            quota_.close();
        }
    private:
        friend class TrustedHierarchicalNetworkGate;
        TrustedHierarchicalNetworkGate* gate_;
        Quota quota_;
    };

    class Lease final {
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : gate_(other.gate_), child_(std::move(other.child_)),
              supervisor_(std::move(other.supervisor_)),
              tenant_(std::move(other.tenant_)),
              state_(other.state_.exchange(2)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                gate_ = other.gate_;
                child_ = std::move(other.child_);
                supervisor_ = std::move(other.supervisor_);
                tenant_ = std::move(other.tenant_);
                state_.store(other.state_.exchange(2));
            }
            return *this;
        }
        ~Lease() noexcept { release(); }

        bool commit_dispatch() noexcept {
            if (!gate_) return false;
            std::lock_guard<std::mutex> guard(gate_->lock_);
            std::uint8_t expected = 0;
            if (!state_.compare_exchange_strong(expected, 1)) return false;
            if (!child_.commit_dispatch() || !supervisor_.commit_dispatch()
                    || !tenant_.commit_dispatch()) std::abort();
            return true;
        }

        bool abort_before_dispatch(std::uint64_t trusted_nanos) noexcept {
            if (!gate_) return false;
            std::lock_guard<std::mutex> guard(gate_->lock_);
            std::uint8_t expected = 0;
            if (!state_.compare_exchange_strong(expected, 2)) return false;
            if (!child_.abort_before_dispatch(trusted_nanos)
                    || !supervisor_.abort_before_dispatch(trusted_nanos)
                    || !tenant_.abort_before_dispatch(trusted_nanos)) std::abort();
            return true;
        }

        void release() noexcept {
            if (!gate_) return;
            std::lock_guard<std::mutex> guard(gate_->lock_);
            if (state_.exchange(2) == 2) return;
            child_.release();
            supervisor_.release();
            tenant_.release();
        }
    private:
        friend class TrustedHierarchicalNetworkGate;
        Lease(TrustedHierarchicalNetworkGate& gate, Quota::Lease&& a,
              Quota::Lease&& b, Quota::Lease&& c) noexcept
            : gate_(&gate), child_(std::move(a)), supervisor_(std::move(b)),
              tenant_(std::move(c)), state_(0) {}
        TrustedHierarchicalNetworkGate* gate_ = nullptr;
        Quota::Lease child_, supervisor_, tenant_;
        std::atomic<std::uint8_t> state_{2}; // 0=reserved,1=dispatched,2=settled
    };

    struct Admission {
        Status status;
        Lease lease;
    };

    Admission try_start_http(Scope& child, Scope& supervisor, Scope& tenant,
                             std::uint64_t trusted_nanos) noexcept {
        return attempt(child, supervisor, tenant, trusted_nanos, true);
    }

    Admission try_open_tcp(Scope& child, Scope& supervisor, Scope& tenant,
                           std::uint64_t trusted_nanos) noexcept {
        return attempt(child, supervisor, tenant, trusted_nanos, false);
    }

private:
    Admission attempt(Scope& child, Scope& supervisor, Scope& tenant,
                      std::uint64_t now, bool http) noexcept {
        if (&child == &supervisor || &child == &tenant || &supervisor == &tenant
                || child.gate_ != this || supervisor.gate_ != this || tenant.gate_ != this)
            return {Status::invalid_policy, Lease{}};
        std::lock_guard<std::mutex> guard(lock_);
        auto first = admit(child.quota_, now, http);
        if (first.status != Status::admitted) return {first.status, Lease{}};
        auto second = admit(supervisor.quota_, now, http);
        if (second.status != Status::admitted) {
            if (!first.lease.abort_before_dispatch(now)) std::abort();
            return {second.status, Lease{}};
        }
        auto third = admit(tenant.quota_, now, http);
        if (third.status != Status::admitted) {
            if (!second.lease.abort_before_dispatch(now)
                    || !first.lease.abort_before_dispatch(now)) std::abort();
            return {third.status, Lease{}};
        }
        return {Status::admitted, Lease(*this, std::move(first.lease),
                                      std::move(second.lease), std::move(third.lease))};
    }

    static Quota::Admission admit(Quota& quota, std::uint64_t now, bool http) noexcept {
        return http ? quota.try_start_http(now) : quota.try_open_tcp(now);
    }
    std::mutex lock_;
};
} // namespace oreslang::runtime
