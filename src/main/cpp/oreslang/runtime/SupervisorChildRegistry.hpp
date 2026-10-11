#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted host bookkeeping for logical child identity, distinct from the
 * current native actor (slot, generation). No guest authority or restart
 * implementation is conferred by this registry.
 *
 * The actor lifecycle owner MUST prove physical finalization before marking
 * a generation finalized and MUST have admitted a live replacement before
 * calling replace(). Neither operation is performed here.
 */
class SupervisorChildRegistry final {
public:
    struct ChildRef {
        std::uint64_t supervisor_id;
        std::uint64_t runtime_id;
        std::uint64_t logical_child_id;
        bool operator==(const ChildRef& rhs) const noexcept {
            return supervisor_id == rhs.supervisor_id
                && runtime_id == rhs.runtime_id
                && logical_child_id == rhs.logical_child_id;
        }
    };
    enum class Phase { active, finalized, restarting, stopped };
    struct RestartReservation {
        ChildRef child;
        OresNativeActorRef failed_actor;
        std::uint64_t incarnation;
        std::uint64_t nonce;
    };
    struct Snapshot {
        ChildRef child;
        OresNativeActorRef actor;
        std::uint64_t incarnation;
        Phase phase;
    };

    SupervisorChildRegistry(std::uint64_t supervisor_id, std::uint64_t runtime_id)
        : supervisor_id_(supervisor_id), runtime_id_(runtime_id) {
        if (!supervisor_id_ || !runtime_id_)
            throw std::invalid_argument("invalid supervisor child registry");
    }

    Snapshot register_child(OresNativeActorRef actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        validate_actor(actor);
        if (active_actors_.count(actor_key(actor)))
            throw std::logic_error("native actor already bound to a logical child");
        ChildRef child{supervisor_id_, runtime_id_, fresh_child_id()};
        Snapshot first{child, actor, 1, Phase::active};
        // A previously retired native actor slot+generation cannot be
        // republished as a fresh incarnation (A -> B -> A). Remember only
        // the highest generation for each slot, not every old actor.
        require_fresh_generation(actor);
        auto [seen_it, new_slot] = highest_generation_.try_emplace(
            actor.actor_slot, 0);
        try {
            auto [active_it, inserted] =
                active_actors_.emplace(actor_key(actor), child.logical_child_id);
            (void) active_it;
            if (!inserted)
                throw std::logic_error("actor already belongs to a logical child");
            children_.emplace(child.logical_child_id, first);
            seen_it->second = actor.generation;
        } catch (...) {
            children_.erase(child.logical_child_id);
            active_actors_.erase(actor_key(actor));
            if (new_slot) highest_generation_.erase(seen_it);
            throw;
        }
        return first;
    }

    Snapshot snapshot(ChildRef child) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return require_current(child);
    }

    Snapshot finalized(ChildRef child, OresNativeActorRef actor,
                       std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto current = require_expected(
            child, actor, incarnation, Phase::active);
        active_actors_.erase(actor_key(current.actor));
        auto& stored = children_.at(child.logical_child_id);
        stored.phase = Phase::finalized;
        return stored;
    }

    /**
     * Claim the current finalized generation before the host creates an
     * actor. Only one claimant may spawn a replacement for that failure.
     */
    RestartReservation begin_restart(
            ChildRef child, OresNativeActorRef failed_actor,
            std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, incarnation, Phase::finalized);
        if (incarnation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("logical child incarnation exhausted");
        RestartReservation permit{
            child, failed_actor, incarnation, fresh_restart_nonce()};
        auto [it, inserted] = pending_restarts_.emplace(
            child.logical_child_id, permit.nonce);
        (void) it;
        if (!inserted) throw std::logic_error("restart reservation already exists");
        children_.at(child.logical_child_id).phase = Phase::restarting;
        return permit;
    }

    /** Identity commit only; caller must have admitted a fresh actor. */
    Snapshot replace(RestartReservation permit, OresNativeActorRef replacement) {
        std::lock_guard<std::mutex> guard(mutex_);
        check_reservation(permit);
        validate_actor(replacement);
        if (same_actor(replacement, permit.failed_actor)
            || active_actors_.count(actor_key(replacement)))
            throw std::logic_error("native replacement actor is not fresh");
        require_fresh_generation(replacement);
        // Stage both the slot replay fence and live actor index before
        // changing the child state. An allocation failure preserves the
        // reservation so the host can stop and roll back the spawned actor.
        auto [seen_it, new_slot] = highest_generation_.try_emplace(
            replacement.actor_slot, 0);
        try {
            auto [active_it, inserted] = active_actors_.emplace(
                actor_key(replacement), permit.child.logical_child_id);
            (void) active_it;
            if (!inserted)
                throw std::logic_error("replacement actor already registered");
        } catch (...) {
            if (new_slot) highest_generation_.erase(seen_it);
            throw;
        }
        auto& stored = children_.at(permit.child.logical_child_id);
        stored.actor = replacement;
        stored.incarnation = permit.incarnation + 1;
        stored.phase = Phase::active;
        seen_it->second = replacement.generation;
        pending_restarts_.erase(permit.child.logical_child_id);
        return stored;
    }

    /** Trusted owner rolls back failed spawn/admission, never guest code. */
    Snapshot abort_restart(RestartReservation permit) {
        std::lock_guard<std::mutex> guard(mutex_);
        check_reservation(permit);
        auto& stored = children_.at(permit.child.logical_child_id);
        stored.phase = Phase::finalized;
        pending_restarts_.erase(permit.child.logical_child_id);
        return stored;
    }

    Snapshot stop(ChildRef child, OresNativeActorRef failed_actor,
                  std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, incarnation, Phase::finalized);
        auto& stored = children_.at(child.logical_child_id);
        stored.phase = Phase::stopped;
        return stored;
    }

    void retire_stopped(ChildRef child) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto current = require_current(child);
        if (current.phase != Phase::stopped)
            throw std::logic_error("cannot retire active/restartable child");
        children_.erase(child.logical_child_id);
    }

    std::size_t tracked_children() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return children_.size();
    }

private:
    void check_reservation(const RestartReservation& permit) const {
        require_expected(permit.child, permit.failed_actor,
                         permit.incarnation, Phase::restarting);
        const auto it = pending_restarts_.find(permit.child.logical_child_id);
        if (!permit.nonce || it == pending_restarts_.end()
            || it->second != permit.nonce)
            throw std::logic_error("stale or foreign restart reservation");
    }

    static std::uint64_t fresh_restart_nonce() {
        auto current = next_restart_nonce_.load(std::memory_order_relaxed);
        while (true) {
            if (current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("restart reservation nonce exhausted");
            if (next_restart_nonce_.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed))
                return current + 1;
        }
    }

    // Native logical-child IDs are process-unique, not registry-local.
    // Otherwise a replacement supervisor registry with the same owner IDs
    // could reuse child 1, letting a stale ChildRef target unrelated work.
    static std::uint64_t fresh_child_id() {
        auto current = next_global_child_id_.load(std::memory_order_relaxed);
        while (true) {
            if (current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("logical child ID exhausted");
            if (next_global_child_id_.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed))
                return current + 1;
        }
    }
    static bool same_actor(OresNativeActorRef a, OresNativeActorRef b) noexcept {
        return a.scheduler_domain == b.scheduler_domain
            && a.actor_slot == b.actor_slot && a.generation == b.generation;
    }
    void require_fresh_generation(OresNativeActorRef actor) const {
        const auto slot = highest_generation_.find(actor.actor_slot);
        if (slot != highest_generation_.end()
            && actor.generation <= slot->second)
            throw std::logic_error("native actor slot generation was already used");
        const auto live = active_actors_.lower_bound({actor.actor_slot, 0});
        if (live != active_actors_.end()
            && live->first.first == actor.actor_slot)
            throw std::logic_error("native actor slot already has a live incarnation");
    }
    static std::pair<std::uint64_t, std::uint64_t>
    actor_key(OresNativeActorRef actor) noexcept {
        return {actor.actor_slot, actor.generation};
    }
    void validate_actor(OresNativeActorRef actor) const {
        if (actor.scheduler_domain != runtime_id_ || !actor.generation)
            throw std::invalid_argument("foreign or invalid actor incarnation");
    }
    Snapshot require_current(ChildRef child) const {
        if (child.supervisor_id != supervisor_id_
            || child.runtime_id != runtime_id_ || !child.logical_child_id)
            throw std::invalid_argument("foreign supervisor child identity");
        auto found = children_.find(child.logical_child_id);
        if (found == children_.end() || !(found->second.child == child))
            throw std::logic_error("unknown or retired logical child");
        return found->second;
    }
    Snapshot require_expected(ChildRef child, OresNativeActorRef actor,
                              std::uint64_t incarnation, Phase phase) const {
        auto state = require_current(child);
        if (!same_actor(state.actor, actor) || state.incarnation != incarnation
            || state.phase != phase)
            throw std::logic_error("stale child incarnation or lifecycle phase");
        return state;
    }

    const std::uint64_t supervisor_id_;
    const std::uint64_t runtime_id_;
    mutable std::mutex mutex_;
    inline static std::atomic<std::uint64_t> next_global_child_id_{0};
    inline static std::atomic<std::uint64_t> next_restart_nonce_{0};
    std::unordered_map<std::uint64_t, std::uint64_t> pending_restarts_;
    std::unordered_map<std::uint64_t, Snapshot> children_;
    // Bounded by distinct native scheduler slots, not restart count.
    std::map<std::uint64_t, std::uint64_t> highest_generation_;
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             std::uint64_t> active_actors_;
};

} // namespace oreslang::runtime
