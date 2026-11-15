#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeRegionRuntimeAuthority.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

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
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, ref))
            throw std::logic_error("native pointer actor ref is not live/issued by scheduler");
        authority_.admit_actor(identity(ref), admission);
    }

    // Host code in an authenticated scheduler callback is allowed to request
    // an already-authorized region. External host threads, foreign carriers,
    // stale actor refs and STOPPING actors are rejected. The base pointer must
    // still come from a supervisor-owned mapped allocation; arbitrary native
    // host C++ remains inside the trusted computing base.
    Grant issue_current_region(OresNativeActorRef ref,
                               std::uint64_t region,
                               std::uint64_t allocation,
                               void* base, std::size_t bytes,
                               Mode mode) {
        require_current_live(ref);
        return authority_.issue_region(identity(ref), region, allocation,
                                       base, bytes, mode);
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
        const auto actor = identity(ref);
        // A STOP from another subsystem may already have sealed the mailbox.
        // Still fence the authenticated incarnation to prevent stale cursors.
        (void)ores_native_actor_scheduler_stop_sealed(scheduler_, ref);
        authority_.retire_actor(actor);
    }

    // Caller has already observed a terminal ref (panic, raise escalation or
    // external STOP). Revocation must finish before backing memory is freed.
    void revoke_terminal(OresNativeActorRef ref) {
        if (ores_native_actor_scheduler_in_any_actor_turn())
            throw std::logic_error("native pointer terminal revoke forbidden on actor carrier");
        if (!ores_native_actor_scheduler_poll_done(scheduler_, ref))
            throw std::logic_error("native pointer actor is not terminal");
        authority_.retire_actor(identity(ref));
    }

private:
    ActorHeapIdentity identity(OresNativeActorRef ref) const {
        if (!ref.scheduler_domain || !ref.generation)
            throw std::logic_error("invalid native scheduler actor ref");
        return ActorHeapIdentity{runtime_id_, ref.actor_slot, ref.generation};
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
};

} // namespace oreslang::runtime
