#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeRegionRuntimeAuthority.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace oreslang::runtime {

// Host-only adapter to the real native scheduler's generation-checked actor
// refs and exact-current-actor admission check. No AttoLang guest bindings,
// grammar, raw address export or privileged source operations are provided.
//
// The embedding VM owns this object and the scheduler. It must route every
// actor stop, panic, raise escalation, trust downgrade, hot-reload retirement
// and runtime teardown through the revocation path before reclaiming memory.
// Generic scheduler APIs by themselves do not yet guarantee that integration.
class NativeActorSupervisor final {
public:
    using Admission = NativeRegionRuntimeAuthority::Admission;
    using Grant = NativeRegionRegistry::Grant;
    using Mode = TrustedPointerWindow::Mode;

    explicit NativeActorSupervisor(OresNativeActorScheduler* scheduler,
                                   std::uint64_t runtime_id,
                                   std::size_t maximum_incarnations = 65536)
        : scheduler_(scheduler),
          runtime_id_(runtime_id),
          authority_(regions_, runtime_id, maximum_incarnations) {
        if (!scheduler_) throw std::invalid_argument("native pointer supervisor scheduler is null");
    }
    NativeActorSupervisor(const NativeActorSupervisor&) = delete;
    NativeActorSupervisor& operator=(const NativeActorSupervisor&) = delete;

    // Trusted VM bootstrapping only. Admission must use a scheduler-issued,
    // currently ready ref. The trust decision is made by the embedding VM;
    // passing "TrustedNative" from guest code is never authorization.
    void admit_spawned(OresNativeActorRef ref, Admission admission) {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error("native pointer actor admission is supervisor-only");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, ref))
            throw std::logic_error("native pointer actor ref is not live/issued by scheduler");
        const auto actor = identity(ref);
        std::lock_guard<std::mutex> lock(admission_mu_);
        const auto key = std::make_pair(actor.actor, actor.epoch);
        const auto [it, inserted] = admitted_refs_.emplace(key, ref);
        if (!inserted)
            throw std::logic_error("native pointer actor ref already registered");
        try {
            authority_.admit_actor(actor, admission);
        } catch (...) {
            admitted_refs_.erase(it);
            throw;
        }
    }

    // Trusted HOST supervisor bootstrapping only: no guest binding. This
    // maps an already owned and live native region into the broker's catalog.
    // An actor turn cannot supply an arbitrary void* during grant issuance.
    void register_mapped_region(OresNativeActorRef ref,
                                std::uint64_t region,
                                std::uint64_t allocation,
                                void* base, std::size_t bytes,
                                Mode mode) {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error("native region registration requires external supervisor");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        require_admitted_ref(ref);
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, ref))
            throw std::logic_error("native mapped region requires live actor");
        authority_.require_trusted_admission(identity(ref));
        if (!region || !allocation || !base || !bytes)
            throw std::invalid_argument("native mapped region is empty/invalid");
        // Device MMIO must use a separate hardware-scoped authority with
        // ordering/width/barrier proof; a RAM grant is never an MMIO grant.
        if (mode != Mode::ReadOnly && mode != Mode::ReadWrite)
            throw std::logic_error("native MMIO mapping requires separate device authority");
        const auto begin = reinterpret_cast<std::uintptr_t>(base);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
            throw std::invalid_argument("native mapped region address overflows");
        const auto end = begin + bytes;
        const auto key = region_key(identity(ref), region, allocation);
        std::lock_guard<std::mutex> lock(mapped_mu_);
        if (mapped_regions_.count(key))
            throw std::logic_error("duplicate native mapped region registration");
        if (mapped_regions_.size() >= maximum_region_mappings_)
            throw std::length_error("native mapped-region catalog exhausted");
        // Even independent IDs must not create multiple native authorities
        // over the same RAM interval without a separate sharing contract.
        for (const auto& registered : mapped_regions_) {
            const auto other_begin = reinterpret_cast<std::uintptr_t>(
                registered.second.base);
            const auto other_end = other_begin + registered.second.bytes;
            if (begin < other_end && other_begin < end)
                throw std::logic_error("overlapping supervisor native regions");
        }
        mapped_regions_.emplace(key, MappedRegion{base, bytes, mode});
    }

    // Host code in an authenticated scheduler callback can request an opaque
    // supervisor-approved region ID. External threads, foreign carriers,
    // stale refs and STOPPING actors fail closed. The callback supplies no
    // raw address, length or access mode at grant issuance.
    Grant issue_current_region(OresNativeActorRef ref,
                               std::uint64_t region,
                               std::uint64_t allocation) {
        require_current_live(ref);
        const auto actor = identity(ref);
        MappedRegion mapping;
        {
            std::lock_guard<std::mutex> lock(mapped_mu_);
            const auto it = mapped_regions_.find(region_key(actor, region, allocation));
            if (it == mapped_regions_.end())
                throw std::logic_error("native region not supervisor registered");
            mapping = it->second;
        }
        return authority_.issue_region(actor, region, allocation,
                                       mapping.base, mapping.bytes, mapping.mode);
    }

    // Keep the cursor inside the current host turn. No cursor is returned to
    // a guest or stored in a suspendable continuation.
    template<class T> T read_current(OresNativeActorRef ref, const Grant& grant,
                                     std::size_t offset = 0) {
        require_current_live(ref);
        const auto actor = identity(ref);
        return regions_.at<T>(grant, actor, offset).load(actor);
    }
    template<class T> void write_current(OresNativeActorRef ref, const Grant& grant,
                                         T value, std::size_t offset = 0) {
        require_current_live(ref);
        const auto actor = identity(ref);
        regions_.at<T>(grant, actor, offset).store(actor, value);
    }

    // External supervisor CONTROL path. Seal data admission first, then
    // terminally revoke all native windows for the exact actor incarnation.
    // On active same-thread FFI, retirement throws/defer and must be retried.
    // Does NOT wait for scheduler DONE: the host must separately wait for the
    // exact ref to become terminal before freeing the actor heap.
    void stop_and_revoke(OresNativeActorRef ref) {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error("blocking native pointer retirement forbidden on actor carrier");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        const auto actor = identity(ref);
        require_admitted_ref(ref);
        // A STOP from another subsystem may already have sealed the mailbox.
        // Still fence the authenticated incarnation to prevent stale cursors.
        (void)ores_native_actor_scheduler_stop_sealed(scheduler_, ref);
        authority_.retire_actor(actor);
        forget_mapped_regions(actor);
    }

    // External host maintenance tick: recognize STOP initiated through the
    // ordinary scheduler API, panic paths, global retirement, or other code
    // that did not directly call stop_and_revoke(). The host MUST invoke this
    // before reclaiming actor memory, and repeat until all stopped admitted
    // actors have quiesced. It may block on in-flight synchronous FFI, so it
    // cannot execute on any actor carrier. This is NOT automatic scheduling:
    // production hosts must explicitly install this lifecycle integration.
    std::size_t fence_stopped_actors() {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error(
                "native pointer maintenance cannot run on an actor carrier");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        std::vector<OresNativeActorRef> stopped;
        {
            std::lock_guard<std::mutex> lock(admission_mu_);
            for (const auto& entry : admitted_refs_) {
                if (!ores_native_actor_scheduler_poll_ready(scheduler_, entry.second))
                    stopped.push_back(entry.second);
            }
        }
        std::size_t newly_quiesced = 0;
        for (const auto& ref : stopped) {
            const auto actor = identity(ref);
            if (authority_.actor_quiescent(actor)) continue;
            // If an FFI callback cannot quiesce, this throws: the host MUST
            // retry later, and may not release arena backing bytes meantime.
            authority_.retire_actor(actor);
            forget_mapped_regions(actor);
            ++newly_quiesced;
        }
        return newly_quiesced;
    }

    // Caller has already observed a terminal ref (panic, raise escalation or
    // external STOP). Revocation must finish before backing memory is freed.
    void revoke_terminal(OresNativeActorRef ref) {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error("native pointer terminal revoke forbidden on actor carrier");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        require_admitted_ref(ref);
        if (!ores_native_actor_scheduler_poll_done(scheduler_, ref))
            throw std::logic_error("native pointer actor is not terminal");
        const auto actor = identity(ref);
        authority_.retire_actor(actor);
        forget_mapped_regions(actor);
    }

private:
    struct MappedRegion {
        void* base = nullptr;
        std::size_t bytes = 0;
        Mode mode = Mode::ReadOnly;
    };
    using RegionKey = std::tuple<std::uint64_t, std::uint64_t,
                                 std::uint64_t, std::uint64_t>;
    static RegionKey region_key(ActorHeapIdentity actor,
                                std::uint64_t region,
                                std::uint64_t allocation) noexcept {
        return {actor.actor, actor.epoch, region, allocation};
    }

    void forget_mapped_regions(ActorHeapIdentity actor) {
        std::lock_guard<std::mutex> lock(mapped_mu_);
        for (auto it = mapped_regions_.begin(); it != mapped_regions_.end();) {
            if (std::get<0>(it->first) == actor.actor &&
                std::get<1>(it->first) == actor.epoch)
                it = mapped_regions_.erase(it);
            else ++it;
        }
    }

    ActorHeapIdentity identity(OresNativeActorRef ref) const {
        if (!ref.scheduler_domain || !ref.generation)
            throw std::logic_error("invalid native scheduler actor ref");
        return ActorHeapIdentity{runtime_id_, ref.actor_slot, ref.generation};
    }

    void require_admitted_ref(OresNativeActorRef ref) const {
        const auto actor = identity(ref);
        std::lock_guard<std::mutex> lock(admission_mu_);
        const auto it = admitted_refs_.find({actor.actor, actor.epoch});
        if (it == admitted_refs_.end() ||
            it->second.scheduler_domain != ref.scheduler_domain ||
            it->second.actor_slot != ref.actor_slot ||
            it->second.generation != ref.generation)
            throw std::logic_error("native pointer retirement requires admitted exact scheduler ref");
    }

    void require_current_live(OresNativeActorRef ref) const {
        if (!ores_native_actor_scheduler_is_current_actor(scheduler_, ref) ||
            !ores_native_actor_scheduler_poll_ready(scheduler_, ref))
            throw std::logic_error(
                "native pointer access requires exact current live actor turn");
    }

    OresNativeActorScheduler* scheduler_; // external owner, must outlive bridge
    const std::uint64_t runtime_id_;
    NativeRegionRegistry regions_;
    NativeRegionRuntimeAuthority authority_;
    // Serializes supervisor admission/mapping against STOP, shutdown and
    // removal. Actor callbacks may issue only previously registered regions
    // and never acquire this lock, avoiding FFI callback lock inversion.
    mutable std::mutex lifecycle_mu_;
    mutable std::mutex admission_mu_;
    // Retain exact scheduler-domain witnesses until bridge shutdown. This
    // forbids a forged/stale reference from retiring an unrelated actor.
    std::map<std::pair<std::uint64_t, std::uint64_t>, OresNativeActorRef> admitted_refs_;
    mutable std::mutex mapped_mu_;
    const std::size_t maximum_region_mappings_ = 65536;
    std::map<RegionKey, MappedRegion> mapped_regions_;
};

} // namespace oreslang::runtime
