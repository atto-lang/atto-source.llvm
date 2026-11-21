#pragma once

#include "oreslang/runtime/NativeRegionRegistry.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace oreslang::runtime {

// Defined by the trusted native supervisor integration, never by guest code.
class NativeActorSupervisor;
#ifdef ORESLANG_NATIVE_GRANT_TESTING
struct NativeRegionAuthorityTestAccess;
#endif

// Host-only broker: no source pointer or unsafe lowering is enabled by this.
// The scheduler/supervisor must authenticate actor admission independently.
// This object is a privileged TCB control surface, not a guest-callable API.
class NativeRegionRuntimeAuthority final {
    friend class NativeActorSupervisor;
#ifdef ORESLANG_NATIVE_GRANT_TESTING
    friend struct NativeRegionAuthorityTestAccess;
#endif

public:
    enum class Admission { Untrusted, TrustedNative };

private:
    enum class State { Untrusted, TrustedNative, Retiring, Retired };
    using ActorKey = std::tuple<std::uint64_t, std::uint64_t>;
    NativeRegionRegistry& regions_;
    const std::uint64_t runtime_id_;
    const std::size_t maximum_incarnations_;
    std::mutex mu_;
    // Retired entries remain tombstoned until registry/process retirement:
    // no same-incarnation privilege re-admission after cancellation/restart.
    std::map<ActorKey, State> actors_;

    explicit NativeRegionRuntimeAuthority(NativeRegionRegistry& regions,
                                           std::uint64_t runtime_id,
                                           std::size_t maximum_incarnations = 65536)
        : regions_(regions), runtime_id_(runtime_id),
          maximum_incarnations_(maximum_incarnations) {
        if (!runtime_id || !maximum_incarnations)
            throw std::invalid_argument("native broker runtime id/capacity is zero");
        // The registry may never be rebound to a different broker, even if
        // the first broker is destructed. Otherwise a second broker could
        // re-admit an actor incarnation that the first broker retired.
        regions_.bind_runtime_authority(runtime_id_);
    }
    static ActorKey key(ActorHeapIdentity actor) noexcept {
        return {actor.actor, actor.epoch};
    }
    void verify_actor(ActorHeapIdentity actor) const {
        // Actor ID zero is a valid native allocation-domain identity.
        if (actor.runtime != runtime_id_ || actor.epoch == 0)
            throw std::logic_error("native broker runtime/epoch mismatch");
    }

    // Called only after the native supervisor has established actor identity,
    // execution domain and trust. No self-upgrade from an untrusted actor.
    void admit_actor(ActorHeapIdentity actor, Admission admission) {
        verify_actor(actor);
        if (admission != Admission::Untrusted &&
            admission != Admission::TrustedNative)
            throw std::logic_error("invalid native actor trust category");
        std::lock_guard<std::mutex> lock(mu_);
        // Tombstones are intentionally permanent until broker retirement.
        // Bound total admitted epochs; never reclaim a terminal epoch early.
        if (actors_.size() >= maximum_incarnations_ && !actors_.count(key(actor)))
            throw std::length_error("native broker actor-incarnation budget exhausted");
        const auto state = admission == Admission::TrustedNative
                               ? State::TrustedNative : State::Untrusted;
        if (!actors_.emplace(key(actor), state).second)
            throw std::logic_error("actor incarnation already admitted or retired");
    }

    // Validate supervisor mapping registration before any raw region address
    // is associated with a particular actor. Untrusted/retired actors do not
    // get a reservation that might later be mistaken for an issued grant.
    void require_trusted_admission(ActorHeapIdentity actor) {
        verify_actor(actor);
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = actors_.find(key(actor));
        if (it == actors_.end() || it->second != State::TrustedNative)
            throw std::logic_error("native region registration requires trusted actor");
    }

    // Only the broker, not a client-supplied Trust enum, may invoke private
    // NativeRegionRegistry::issue. The lock linearizes issuance with retirement.
    NativeRegionRegistry::Grant issue_region(
            ActorHeapIdentity actor, std::uint64_t region,
            std::uint64_t allocation, void* base, std::size_t bytes,
            TrustedPointerWindow::Mode mode) {
        verify_actor(actor);
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = actors_.find(key(actor));
        if (it == actors_.end() || it->second != State::TrustedNative)
            throw std::logic_error("actor lacks trusted native pointer admission");
        return regions_.issue(NativeRegionRegistry::Trust::Trusted,
                              actor, region, allocation, base, bytes, mode);
    }

    // Actor shutdown, supervisor cancellation, trust downgrade or permission
    // loss all permanently revoke this incarnation; a new epoch is required
    // for later authority. A deferred same-thread FFI callback throws and
    // MUST be retried after callback exit before unmapping backing bytes.
    void retire_actor(ActorHeapIdentity actor) {
        verify_actor(actor);
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = actors_.find(key(actor));
            if (it == actors_.end())
                throw std::logic_error("unadmitted actor cannot request native retirement");
            // A deferred FFI fence or allocation failure cannot be reported
            // as successful terminal cleanup. Retiring still denies grants.
            it->second = State::Retiring;
        }
        regions_.fence_actor(actor);
        {
            std::lock_guard<std::mutex> lock(mu_);
            actors_.at(key(actor)) = State::Retired;
        }
    }

    bool actor_quiescent(ActorHeapIdentity actor) {
        verify_actor(actor);
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = actors_.find(key(actor));
        return it != actors_.end() && it->second == State::Retired;
    }

    bool shutdown_quiescent() {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& actor : actors_)
            if (actor.second != State::Retired) return false;
        return true;
    }

public:
    // The native allocator must never outlive an unfinished broker retirement
    // and free still-leased regions. Fail-stop rather than silently unmapping
    // while FFI callbacks might hold a raw address.
    ~NativeRegionRuntimeAuthority() noexcept {
        if (!shutdown_quiescent()) std::terminate();
    }
    NativeRegionRuntimeAuthority(const NativeRegionRuntimeAuthority&) = delete;
    NativeRegionRuntimeAuthority& operator=(const NativeRegionRuntimeAuthority&) = delete;
    // The owning native supervisor must quiesce/revoke actors *before*
    // destroying either broker or registry. This reference is non-owning.
};

} // namespace oreslang::runtime
