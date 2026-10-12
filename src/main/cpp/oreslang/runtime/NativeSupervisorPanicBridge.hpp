#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SupervisorChildRegistry.hpp"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace oreslang::runtime {

/**
 * Trusted native host bridge between the existing C actor scheduler's typed
 * parent CONTROL callback and the runtime-owned supervisor process.
 *
 * NOT a guest source-level "on_panic" lowering. The host creates and owns
 * this bridge before registering its C callback. The host must drain the C
 * scheduler and the CONTROL scheduler before destroying the bridge.
 *
 * No guest policy executes from on_panic(): it only copies and admits the
 * typed event into SupervisorProcess's dedicated logical CONTROL lane.
 */
class NativeSupervisorPanicBridge final {
public:
    NativeSupervisorPanicBridge(
            OresNativeActorScheduler* scheduler,
            OresNativeActorRef parent,
            std::shared_ptr<SupervisorProcess> process,
            std::shared_ptr<SupervisorChildRegistry> children = nullptr)
        : scheduler_(scheduler), parent_(parent), process_(std::move(process)),
          child_registry_(children
              ? std::move(children)
              : std::make_shared<SupervisorChildRegistry>(
                    process_ ? process_->supervisor_id() : 0,
                    parent.scheduler_domain)) {
        if (!scheduler_ || !process_ || !valid(parent_)
            || process_->runtime_id() != parent_.scheduler_domain
            || !child_registry_
            || child_registry_->supervisor_id() != process_->supervisor_id()
            || child_registry_->runtime_id() != parent_.scheduler_domain)
            throw std::invalid_argument("supervisor bridge domain mismatch");
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, parent_))
            throw std::invalid_argument("supervisor parent must be live");
    }

    // Teardown invalidates the restart publication fence even when a
    // separately held registry outlives this host bridge.
    ~NativeSupervisorPanicBridge() {
        child_registry_->close_restart_admission();
    }

    /** The host must register exactly the current generation of each child. */
    void register_child(OresNativeActorRef child) {
        if (!valid(child) || child.scheduler_domain != parent_.scheduler_domain
                || child.actor_slot == parent_.actor_slot
                || !ores_native_actor_scheduler_poll_published(scheduler_, child))
            throw std::invalid_argument("foreign, stale or stopped child");
        std::lock_guard<std::mutex> lock(mutex_);
        const Key key{child.actor_slot, child.generation};
        if (registered_.count(key) != 0)
            throw std::logic_error("child generation already registered");
        const auto snapshot = child_registry_->register_child(child);
        try {
            registered_.emplace(key, snapshot);
        } catch (...) {
            child_registry_->detach_active(snapshot.child, child,
                                          snapshot.incarnation);
            throw;
        }
    }

    /**
     * Retire a normally terminated child generation.
     *
     * Only the trusted actor lifecycle owner may classify this exit as
     * normal. Scheduler-owned seal_normal_exit() atomically confirms physical
     * finalization and denies cleanup if a failure has already been published
     * (including a queued but not yet delivered CONTROL notification).
     * It also forbids any later failure publication for the generation.
     *
     * A generation-scoped one-shot erase cannot remove a replacement child,
     * even if the native scheduler reuses the same actor slot.
     * Return false for an active/stale/foreign/unregistered generation.
     * Never accept this API as a guest-visible capability.
     */
    bool retire_normally_finalized_child(OresNativeActorRef child) noexcept {
        if (!valid(child) || child.scheduler_domain != parent_.scheduler_domain
                || child.actor_slot == parent_.actor_slot)
            return false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const Key key{child.actor_slot, child.generation};
            if (registered_.count(key) == 0) return false;
            // Both retirement and on_panic hold the bridge lock before asking
            // the scheduler, so they cannot race to consume the same key.
            // The scheduler seals the child under its own lock, making the
            // absence of a published terminal failure authoritative.
            if (!ores_native_actor_scheduler_seal_normal_exit(scheduler_, child))
                return false;
            const auto admitted = registered_.at(key);
            child_registry_->finalized(admitted.child, child, admitted.incarnation);
            child_registry_->stop(admitted.child, child, admitted.incarnation);
            child_registry_->retire_stopped(admitted.child);
            registered_.erase(key);
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * Invoked only from the C scheduler's typed CONTROL callback. Fail
     * closed on stale/foreign/replayed child events or premature delivery.
     * Return false instead of throwing through a C ABI callback boundary.
     */
    bool on_panic(std::size_t parent_slot,
                  const OresNativeActorTerminalFailure& failure) noexcept {
        try {
            if (parent_slot != parent_.actor_slot
                    || failure.kind < ORES_NATIVE_CONTROL_PANIC
                    || failure.kind > ORES_NATIVE_CONTROL_STOPPED
                    || !valid(failure.child)
                    || failure.child.scheduler_domain != parent_.scheduler_domain)
                return reject();
            Key key{failure.child.actor_slot, failure.child.generation};
            SupervisorChildRegistry::Snapshot admitted;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (registered_.count(key) == 0) {
                    ++rejected_;
                    return false;
                }
                // Preserve finalization-before-supervisor policy ordering.
                // A native child cannot be finalized while its user turn is
                // still executing, even if stop() has been requested.
                if (!ores_native_actor_scheduler_poll_done(scheduler_, failure.child)) {
                    ++rejected_;
                    return false;
                }
                admitted = registered_.at(key);
                // Finalization must be established while the scheduler has
                // confirmed physical child teardown. Capture the exact
                // logical identity before the one-shot key is consumed.
                child_registry_->finalized(
                    admitted.child, failure.child, admitted.incarnation);
                registered_.erase(key);
            }
            SupervisorFailureNotice copied{
                process_->supervisor_id(),
                process_->runtime_id(),
                failure.child.actor_slot,
                failure.child.generation,
                parent_.actor_slot,
                "native terminal failure",
                failure.kind,
                failure.failure_code,
                failure.correlation,
                admitted.child.logical_child_id,
                admitted.incarnation
            };
            process_->accept(copied); // enqueues only, never invokes policy inline
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++accepted_;
            }
            return true;
        } catch (...) {
            return reject(); // process counts its own failed enqueue
        }
    }

    /**
     * Trusted host wiring for the verified source installer. A source STOP
     * policy must share this exact registry with its scheduler panic bridge:
     * another registry for the same owner cannot validate its child IDs.
     * This host-only handle is never exported to guest code.
     */
    std::shared_ptr<SupervisorChildRegistry> trusted_child_registry() const {
        return child_registry_;
    }

    /**
     * Trusted bridge-to-scheduler admission adapter. The real scheduler
     * must know the exact incarnation before the registry can publish it.
     * Guest code never obtains this capability or a mutable admission token.
     */
    SupervisorChildRegistry::Snapshot admit_verified_replacement(
            SupervisorChildRegistry::RestartReservation permit,
            OresNativeActorRef replacement) {
        if (replacement.scheduler_domain != parent_.scheduler_domain)
            throw std::logic_error("foreign replacement scheduler domain");

        // Acquire supervisor CONTROL ownership *before* the scheduler gate.
        // The CONTROL decision fence is also taken by close()/finish().
        // Acquiring these in the opposite order deadlocks when a decision
        // thread is stopping a physical actor while a restart publishes.
        return process_->with_healthy_restart_transaction([&]()
                -> SupervisorChildRegistry::Snapshot {
            OresNativeRestartLease scheduler_lease{};
            if (!ores_native_actor_scheduler_acquire_restart_lease(
                    scheduler_, replacement, &scheduler_lease))
                throw std::logic_error(
                    "replacement has no unpublished scheduler admission");
            struct LeaseScope {
                OresNativeActorScheduler* scheduler;
                OresNativeRestartLease lease;
                ~LeaseScope() {
                    if (!ores_native_actor_scheduler_release_restart_lease(
                            scheduler, lease)) std::terminate();
                }
            } lease_scope{scheduler_, scheduler_lease};

            child_registry_->stage_replacement(permit, replacement);
            child_registry_->acknowledge_replacement_admission(
                permit, replacement, [&] {
                    return ores_native_actor_scheduler_poll_ready(
                        scheduler_, replacement);
                });
            // The bridge lock excludes terminal CONTROL routing until both
            // identity indices publish. The process mutex serializes the
            // entire transaction with CONTROL close/failure classification.
            // The scheduler's internal mutex is never held over these maps.
            std::lock_guard<std::mutex> guard(mutex_);
            const Key key{replacement.actor_slot, replacement.generation};
            const SupervisorChildRegistry::Snapshot candidate{
                permit.child, replacement, permit.incarnation + 1,
                SupervisorChildRegistry::Phase::active};
            auto [it, inserted] = registered_.emplace(key, candidate);
            if (!inserted)
                throw std::logic_error("replacement already supervised");
            try {
                auto committed = child_registry_->replace(permit, replacement);
                it->second = committed; // immutable identity, no allocation
                // The physical actor is pinned until routing is published.
                // Under the exact scheduler lease this is an infallible
                // internal flip; fail closed on any invariant violation.
                if (!ores_native_actor_scheduler_publish_restart_lease(
                        scheduler_, scheduler_lease)) std::terminate();
                return committed;
            } catch (...) {
                registered_.erase(it);
                throw;
            }
        });
    }

    /**
     * On a failed spawn/admission, the scheduler must report physical DONE
     * before the old reservation is released. No automatic orphan cleanup.
     */
    SupervisorChildRegistry::Snapshot abort_cleaned_replacement(
            SupervisorChildRegistry::RestartReservation permit,
            OresNativeActorRef replacement) {
        child_registry_->acknowledge_replacement_cleanup(
            permit, replacement, [&] {
                return ores_native_actor_scheduler_poll_done(scheduler_, replacement);
            });
        return child_registry_->abort_restart(permit);
    }

    /** Terminal supervisor teardown fences any in-flight RESTART commit. */
    void fence_restart_publication() {
        child_registry_->close_restart_admission();
    }

    /** Host-only immutable view; neither ID alone grants guest authority. */
    SupervisorChildRegistry::Snapshot child_snapshot(
            SupervisorChildRegistry::ChildRef child) const {
        return child_registry_->snapshot(child);
    }

    /** Secure a generation-scoped STOP lease before any host side effect. */
    SupervisorChildRegistry::Snapshot reserve_finalized_stop(
            const SupervisorProcess::Resolution& resolution) {
        auto key = validated_stop_child(resolution);
        OresNativeActorRef actor{resolution.notice.runtime_id,
                                 resolution.notice.actor_id,
                                 resolution.notice.generation};
        return child_registry_->begin_stop(
            key, actor, resolution.notice.logical_incarnation,
            resolution.turn_generation);
    }

    /**
     * Host-only, one-use callback-entry claim for externally implemented STOP
     * effects. The caller must invoke this immediately before the actual host
     * effect, not during reservation and not after the acknowledgment.
     * This never treats the invocation as proof of effect success.
     */
    void claim_finalized_stop_host_invocation(
            const SupervisorProcess::Resolution& resolution) {
        const auto key = validated_stop_child(resolution);
        const OresNativeActorRef actor{resolution.notice.runtime_id,
                                      resolution.notice.actor_id,
                                      resolution.notice.generation};
        child_registry_->claim_host_stop_invocation(
            key, actor, resolution.notice.logical_incarnation,
            resolution.turn_generation);
    }

    /** Commit STOP disposition only for an already reserved exact turn. */
    SupervisorChildRegistry::Snapshot acknowledge_finalized_stop(
            const SupervisorProcess::Resolution& resolution) {
        auto key = validated_stop_child(resolution);
        OresNativeActorRef actor{resolution.notice.runtime_id,
                                 resolution.notice.actor_id,
                                 resolution.notice.generation};
        // This entrypoint is called only after the host has authoritatively
        // acknowledged the external STOP effect. Record that receipt for the
        // same physical incarnation before its terminal registry transition.
        child_registry_->record_stop_applied(
            key, actor, resolution.notice.logical_incarnation,
            resolution.turn_generation);
        return child_registry_->commit_stop(
            key, actor, resolution.notice.logical_incarnation,
            resolution.turn_generation);
    }

    // Host-only validation. The logical child ID is never source authority.
    SupervisorChildRegistry::ChildRef validated_stop_child(
            const SupervisorProcess::Resolution& resolution) const {
        const auto& notice = resolution.notice;
        if (resolution.requested != SupervisorProcess::Directive::stop
                || resolution.turn_generation == 0
                || notice.supervisor_id != process_->supervisor_id()
                || notice.runtime_id != process_->runtime_id()
                || notice.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC
                || !notice.actor_id || !notice.generation
                || !notice.logical_child_id || !notice.logical_incarnation) {
            throw std::logic_error("unauthorized finalized supervisor STOP");
        }
        return SupervisorChildRegistry::ChildRef{
            notice.supervisor_id, notice.runtime_id, notice.logical_child_id};
    }

    std::size_t tracked_logical_children() const {
        return child_registry_->tracked_children();
    }

    std::size_t registered_children() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return registered_.size();
    }
    std::size_t accepted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepted_;
    }
    std::size_t rejected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

private:
    struct Key {
        std::uint64_t slot;
        std::uint64_t generation;
        bool operator==(const Key& other) const noexcept {
            return slot == other.slot && generation == other.generation;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            auto first = std::hash<std::uint64_t>{}(key.slot);
            auto second = std::hash<std::uint64_t>{}(key.generation);
            return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
        }
    };
    static bool valid(OresNativeActorRef ref) noexcept {
        return ref.scheduler_domain != 0 && ref.generation != 0;
    }
    bool reject() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejected_;
        return false;
    }

    OresNativeActorScheduler* const scheduler_; // borrowed, externally fenced
    const OresNativeActorRef parent_;
    const std::shared_ptr<SupervisorProcess> process_;
    std::shared_ptr<SupervisorChildRegistry> child_registry_;
    mutable std::mutex mutex_;
    std::unordered_map<Key, SupervisorChildRegistry::Snapshot, KeyHash> registered_;
    std::size_t accepted_ = 0;
    std::size_t rejected_ = 0;
};

} // namespace oreslang::runtime
