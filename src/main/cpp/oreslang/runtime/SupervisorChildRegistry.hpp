#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <atomic>
#include <cstdint>
#include <functional>
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
    enum class Phase { active, finalized, restarting, stopping, stopped };
    enum class ReplacementPhase { spawned, admitted, cleaned };
    struct ReplacementState {
        OresNativeActorRef actor;
        std::uint64_t next_incarnation;
        ReplacementPhase phase;
    };
    enum class StopReceiptStatus { pending, applied, acknowledged, uncertain };
    struct StopReceipt {
        ChildRef child;
        OresNativeActorRef failed_actor;
        std::uint64_t incarnation;
        std::uint64_t turn_generation;
        StopReceiptStatus status;
    };
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

    std::uint64_t supervisor_id() const noexcept { return supervisor_id_; }
    std::uint64_t runtime_id() const noexcept { return runtime_id_; }

    Snapshot register_child(OresNativeActorRef actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        validate_actor(actor);
        if (active_actors_.count(actor_key(actor))
            || staged_slots_.count(actor.actor_slot))
            throw std::logic_error("native actor already bound or staged");
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

    /** Trusted admission rollback; not a physical actor finalization. */
    void detach_active(ChildRef child, OresNativeActorRef actor,
                       std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto current = require_expected(
            child, actor, incarnation, Phase::active);
        active_actors_.erase(actor_key(current.actor));
        children_.erase(child.logical_child_id);
        // highest_generation_ intentionally survives: stale scheduler
        // incarnations must not register again even after detachment.
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
        if (restart_closed_ || replacements_.count(child.logical_child_id))
            throw std::logic_error("supervisor restart admission closed or conflicting");
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

    void close_restart_admission() {
        std::lock_guard<std::mutex> guard(mutex_);
        restart_closed_ = true;
    }

    /**
     * Trusted owner stages the real physical actor immediately after spawn.
     * A fresh actor reference alone is not proof of scheduler admission.
     */
    void stage_replacement(RestartReservation permit, OresNativeActorRef actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        check_reservation(permit);
        if (restart_closed_) throw std::logic_error("supervisor is closed");
        validate_actor(actor);
        if (same_actor(actor, permit.failed_actor)
                || active_actors_.count(actor_key(actor)))
            throw std::logic_error("replacement is not fresh");
        require_fresh_generation(actor);
        if (staged_slots_.count(actor.actor_slot))
            throw std::logic_error("native slot already staged by another child");
        auto [slot_it, slot_added] = staged_slots_.emplace(
            actor.actor_slot, permit.child.logical_child_id);
        if (!slot_added) throw std::logic_error("native slot already staged");
        try {
            const auto [it, added] = replacements_.emplace(
                permit.child.logical_child_id,
                ReplacementState{actor, permit.incarnation + 1,
                                 ReplacementPhase::spawned});
            (void) it;
            if (!added) throw std::logic_error("replacement already staged");
        } catch (...) {
            staged_slots_.erase(slot_it);
            throw;
        }
    }

private:
    // Lock-required implementation detail. Never expose mutable registry
    // indexes through a callable public method without acquiring mutex_.
    ReplacementState require_candidate_locked(RestartReservation permit,
                                              OresNativeActorRef actor) const {
        check_reservation(permit);
        auto found = replacements_.find(permit.child.logical_child_id);
        if (found == replacements_.end()
            || !same_actor(found->second.actor, actor)
            || found->second.next_incarnation != permit.incarnation + 1)
            throw std::logic_error("missing or foreign replacement generation");
        return found->second;
    }

public:
    void acknowledge_replacement_admission(
            RestartReservation permit, OresNativeActorRef actor,
            const std::function<bool()>& verify_admitted) {
        if (!verify_admitted)
            throw std::invalid_argument("missing trusted scheduler admission proof");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (restart_closed_ || require_candidate_locked(permit, actor).phase
                    != ReplacementPhase::spawned)
                throw std::logic_error("cannot admit this replacement");
        }
        // External probe must not run holding registry's mutex.
        if (!verify_admitted()) throw std::logic_error("replacement not physically admitted");
        std::lock_guard<std::mutex> guard(mutex_);
        if (restart_closed_ || require_candidate_locked(permit, actor).phase
                != ReplacementPhase::spawned)
            throw std::logic_error("replacement admission became stale");
        replacements_.at(permit.child.logical_child_id).phase
            = ReplacementPhase::admitted;
    }

    void acknowledge_replacement_cleanup(
            RestartReservation permit, OresNativeActorRef actor,
            const std::function<bool()>& verify_cleaned) {
        if (!verify_cleaned)
            throw std::invalid_argument("missing trusted physical cleanup proof");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto status = require_candidate_locked(permit, actor).phase;
            if (status != ReplacementPhase::spawned
                    && status != ReplacementPhase::admitted)
                throw std::logic_error("replacement no longer live");
        }
        if (!verify_cleaned()) throw std::logic_error("replacement still physically live");
        std::lock_guard<std::mutex> guard(mutex_);
        auto status = require_candidate_locked(permit, actor).phase;
        if (status != ReplacementPhase::spawned
                && status != ReplacementPhase::admitted)
            throw std::logic_error("replacement cleanup became stale");
        replacements_.at(permit.child.logical_child_id).phase
            = ReplacementPhase::cleaned;
    }

    /** Identity commit only; caller must have admitted a fresh actor. */
    Snapshot replace(RestartReservation permit, OresNativeActorRef replacement) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto candidate = require_candidate_locked(permit, replacement);
        if (restart_closed_ || candidate.phase != ReplacementPhase::admitted)
            throw std::logic_error("replacement lacks physical admission or supervisor closed");
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
        staged_slots_.erase(replacement.actor_slot);
        replacements_.erase(permit.child.logical_child_id);
        pending_restarts_.erase(permit.child.logical_child_id);
        return stored;
    }

    /** Trusted owner rolls back failed spawn/admission, never guest code. */
    Snapshot abort_restart(RestartReservation permit) {
        std::lock_guard<std::mutex> guard(mutex_);
        check_reservation(permit);
        const auto candidate = replacements_.find(permit.child.logical_child_id);
        if (candidate != replacements_.end()
            && candidate->second.phase != ReplacementPhase::cleaned)
            throw std::logic_error("replacement physical cleanup required before abort");
        auto& stored = children_.at(permit.child.logical_child_id);
        stored.phase = Phase::finalized;
        if (candidate != replacements_.end())
            staged_slots_.erase(candidate->second.actor.actor_slot);
        replacements_.erase(permit.child.logical_child_id);
        pending_restarts_.erase(permit.child.logical_child_id);
        return stored;
    }

    /**
     * Reserve STOP before invoking any host lifecycle effect. A competing
     * restart cannot consume this physically finalized generation. Unknown
     * effects leave the child stopping until explicit reconciliation.
     */
    Snapshot begin_stop(ChildRef child, OresNativeActorRef failed_actor,
                        std::uint64_t incarnation, std::uint64_t turn_generation) {
        if (!turn_generation)
            throw std::invalid_argument("missing STOP turn generation");
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, incarnation, Phase::finalized);
        auto [it, inserted] = pending_stops_.emplace(
            child.logical_child_id, turn_generation);
        (void) it;
        if (!inserted) throw std::logic_error("duplicate STOP turn reservation");
        try {
            auto [receipt_it, inserted_receipt] = stop_receipts_.emplace(
                child.logical_child_id,
                StopReceipt{child, failed_actor, incarnation, turn_generation,
                            StopReceiptStatus::pending});
            (void) receipt_it;
            if (!inserted_receipt)
                throw std::logic_error("stale STOP receipt prevented reservation");
        } catch (...) {
            // No partially published STOP lease or overwritten old receipt.
            pending_stops_.erase(child.logical_child_id);
            throw;
        }
        auto& state = children_.at(child.logical_child_id);
        state.phase = Phase::stopping;
        return state;
    }

    /** Observability is immutable and keyed to the same physical generation. */
    StopReceipt stop_receipt(ChildRef child) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_current(child);
        auto found = stop_receipts_.find(child.logical_child_id);
        if (found == stop_receipts_.end())
            throw std::logic_error("no STOP receipt for logical child");
        return found->second;
    }

    /** Host has independently acknowledged successful STOP application. */
    StopReceipt record_stop_applied(
            ChildRef child, OresNativeActorRef actor,
            std::uint64_t incarnation, std::uint64_t turn_generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& receipt = require_stop_receipt(child, actor, incarnation, turn_generation);
        // The first host callback may acknowledge a PENDING request.
        // An UNCERTAIN receipt may only advance after independent proof in
        // reconcile_uncertain_stop(), never through this direct shortcut.
        if (receipt.status != StopReceiptStatus::pending)
            throw std::logic_error(
                "STOP receipt is not pending; uncertain effects require reconciliation");
        receipt.status = StopReceiptStatus::applied;
        return receipt;
    }

    /** Outcome unknown: retain STOPPING and deny lifecycle retry. */
    StopReceipt record_stop_uncertain(
            ChildRef child, OresNativeActorRef actor,
            std::uint64_t incarnation, std::uint64_t turn_generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& receipt = require_stop_receipt(child, actor, incarnation, turn_generation);
        if (receipt.status == StopReceiptStatus::applied
                || receipt.status == StopReceiptStatus::acknowledged)
            return receipt;
        receipt.status = StopReceiptStatus::uncertain;
        return receipt;
    }

    /**
     * Reconcile a PENDING or UNCERTAIN STOP ONLY after an independent authoritative
     * host receipt check. No automatic retry and no rollback to FINALIZED.
     * The verifier runs outside the registry lock. A stale/changed turn is
     * rejected again under the lock before the disposition can commit.
     */
    bool reconcile_unresolved_stop(
            ChildRef child, OresNativeActorRef actor,
            std::uint64_t incarnation, std::uint64_t turn_generation,
            const std::function<bool(const StopReceipt&)>& verify_applied) {
        if (!verify_applied)
            throw std::invalid_argument("missing independent STOP verifier");
        const auto receipt = stop_receipt(child);
        if ((receipt.status != StopReceiptStatus::uncertain
                && receipt.status != StopReceiptStatus::pending)
                || receipt.incarnation != incarnation
                || receipt.turn_generation != turn_generation
                || !same_actor(receipt.failed_actor, actor))
            throw std::logic_error("foreign or non-uncertain STOP reconciliation");
        if (!verify_applied(receipt))
            return false;  // UNKNOWN remains quarantined; no state change.
        // Recheck the SAME turn under one lock and commit atomically after
        // external verification. Never reopen the direct APPLIED shortcut
        // for an UNKNOWN effect, even if two reconcilers race.
        std::lock_guard<std::mutex> guard(mutex_);
        auto& verified = require_stop_receipt(
            child, actor, incarnation, turn_generation);
        if (verified.status != receipt.status
                || (verified.status != StopReceiptStatus::uncertain
                    && verified.status != StopReceiptStatus::pending))
            throw std::logic_error("STOP changed while being reconciled");
        auto& state = children_.at(child.logical_child_id);
        state.phase = Phase::stopped;
        verified.status = StopReceiptStatus::acknowledged;
        pending_stops_.erase(child.logical_child_id);
        return true;
    }

    /**
     * Compatibility entrypoint for receipts already classified UNCERTAIN.
     * PENDING crash recovery must explicitly use reconcile_unresolved_stop.
     */
    bool reconcile_uncertain_stop(
            ChildRef child, OresNativeActorRef actor,
            std::uint64_t incarnation, std::uint64_t turn_generation,
            const std::function<bool(const StopReceipt&)>& verify_applied) {
        if (stop_receipt(child).status != StopReceiptStatus::uncertain)
            throw std::logic_error("only UNCERTAIN STOP accepted here");
        return reconcile_unresolved_stop(
            child, actor, incarnation, turn_generation, verify_applied);
    }

    Snapshot commit_stop(ChildRef child, OresNativeActorRef failed_actor,
                         std::uint64_t incarnation, std::uint64_t turn_generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, incarnation, Phase::stopping);
        auto found = pending_stops_.find(child.logical_child_id);
        if (!turn_generation || found == pending_stops_.end()
            || found->second != turn_generation)
            throw std::logic_error("stale or foreign STOP turn");
        auto& receipt = require_stop_receipt(child, failed_actor, incarnation,
                                             turn_generation);
        if (receipt.status != StopReceiptStatus::applied)
            throw std::logic_error("STOP lacks authoritative host acknowledgment");
        auto& state = children_.at(child.logical_child_id);
        state.phase = Phase::stopped;
        receipt.status = StopReceiptStatus::acknowledged;
        pending_stops_.erase(found);
        return state;
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
        stop_receipts_.erase(child.logical_child_id);
    }

    std::size_t tracked_children() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return children_.size();
    }

private:
    StopReceipt& require_stop_receipt(
            ChildRef child, OresNativeActorRef actor,
            std::uint64_t incarnation, std::uint64_t turn_generation) {
        require_expected(child, actor, incarnation, Phase::stopping);
        const auto pending = pending_stops_.find(child.logical_child_id);
        if (!turn_generation || pending == pending_stops_.end()
                || pending->second != turn_generation)
            throw std::logic_error("wrong STOP turn generation");
        auto found = stop_receipts_.find(child.logical_child_id);
        if (found == stop_receipts_.end()
            || !(found->second.child == child)
            || found->second.turn_generation != turn_generation
            || found->second.incarnation != incarnation
            || !same_actor(found->second.failed_actor, actor))
            throw std::logic_error("missing or forged STOP receipt");
        return found->second;
    }

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
    bool restart_closed_ = false;
    std::unordered_map<std::uint64_t, ReplacementState> replacements_;
    std::unordered_map<std::uint64_t, std::uint64_t> staged_slots_;
    std::unordered_map<std::uint64_t, std::uint64_t> pending_restarts_;
    std::unordered_map<std::uint64_t, std::uint64_t> pending_stops_;
    std::unordered_map<std::uint64_t, StopReceipt> stop_receipts_;
    std::unordered_map<std::uint64_t, Snapshot> children_;
    // Bounded by distinct native scheduler slots, not restart count.
    std::map<std::uint64_t, std::uint64_t> highest_generation_;
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             std::uint64_t> active_actors_;
};

} // namespace oreslang::runtime
