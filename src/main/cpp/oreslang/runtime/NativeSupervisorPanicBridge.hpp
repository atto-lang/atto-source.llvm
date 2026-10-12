#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeTerminalReceiptJournal.hpp"
#include "oreslang/runtime/SupervisorChildRegistry.hpp"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
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
            std::shared_ptr<SupervisorChildRegistry> children = nullptr,
            std::shared_ptr<NativeTerminalReceiptJournal> durable_journal = nullptr)
        : scheduler_(scheduler), parent_(parent), process_(std::move(process)),
          child_registry_(children
              ? std::move(children)
              : std::make_shared<SupervisorChildRegistry>(
                    process_ ? process_->supervisor_id() : 0,
                    parent.scheduler_domain)),
          durable_journal_(std::move(durable_journal)) {
        if (!scheduler_ || !process_ || !valid(parent_)
            || process_->runtime_id() != parent_.scheduler_domain
            || !child_registry_
            || child_registry_->supervisor_id() != process_->supervisor_id()
            || child_registry_->runtime_id() != parent_.scheduler_domain)
            throw std::invalid_argument("supervisor bridge domain mismatch");
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, parent_))
            throw std::invalid_argument("supervisor parent must be live");
        if (durable_journal_
            && !ores_native_actor_scheduler_uses_identity_callbacks(scheduler_))
            throw std::invalid_argument(
                "durable receipts require generation-aware callback mode");
    }

    // Teardown invalidates the restart publication fence even when a
    // separately held registry outlives this host bridge.
    ~NativeSupervisorPanicBridge() {
        child_registry_->close_restart_admission();
    }

    /** The host must register exactly the current generation of each child. */
    void register_child(OresNativeActorRef child) {
        if (!valid(child) || child.scheduler_domain != parent_.scheduler_domain
                || child.actor_slot == parent_.actor_slot)
            throw std::invalid_argument("foreign or invalid initial child");
        // Initial registration has the same physical and CONTROL close race
        // as RESTART: a readiness observation alone is insufficient.
        process_->with_healthy_restart_transaction([&] {
            OresNativeRestartLease lease{};
            if (!ores_native_actor_scheduler_acquire_registration_lease(
                    scheduler_, child, &lease))
                throw std::invalid_argument("child was not scheduler-published");
            struct LeaseScope {
                OresNativeActorScheduler* scheduler;
                OresNativeRestartLease lease;
                ~LeaseScope() {
                    if (!ores_native_actor_scheduler_release_restart_lease(
                            scheduler, lease)) std::terminate();
                }
            } lease_scope{scheduler_, lease};
            if (!ores_native_actor_scheduler_poll_published(scheduler_, parent_))
                throw std::logic_error("supervisor parent physically retired");
            std::lock_guard<std::mutex> guard(mutex_);
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
        });
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
    /**
     * Generation-bearing PANIC entrypoint for identity-mode native callbacks.
     * Compare the COMPLETE parent incarnation, not only the physical index:
     * a stale parent carrier may not route terminal evidence into a newer
     * supervisor that later occupies the same scheduler lane.
     */
    bool on_panic(OresNativeActorRef parent,
                  const OresNativeActorTerminalFailure& failure) noexcept {
        if (!valid(parent) || parent.scheduler_domain != parent_.scheduler_domain
                || parent.actor_slot != parent_.actor_slot
                || parent.generation != parent_.generation
                || !ores_native_actor_scheduler_uses_identity_callbacks(scheduler_)
                || !ores_native_actor_scheduler_poll_published(scheduler_, parent))
            return reject();
        return on_panic_checked(static_cast<std::size_t>(parent.actor_slot), failure);
    }

    /* Legacy callback entry point. Calling this with an identity-mode
     * scheduler would drop the parent's generation and bypass the stricter
     * callback fence, so reject it before touching logical child state. */
    bool on_panic(std::size_t parent_slot,
                  const OresNativeActorTerminalFailure& failure) noexcept {
        if (ores_native_actor_scheduler_uses_identity_callbacks(scheduler_)
                || !ores_native_actor_scheduler_poll_published(scheduler_, parent_))
            return reject();
        return on_panic_checked(parent_slot, failure);
    }

private:
    bool on_panic_checked(std::size_t parent_slot,
                          const OresNativeActorTerminalFailure& failure) noexcept {
        try {
            if (parent_slot != parent_.actor_slot
                    || failure.kind < ORES_NATIVE_CONTROL_PANIC
                    || failure.kind > ORES_NATIVE_CONTROL_STOPPED
                    || !valid(failure.child)
                    || failure.child.scheduler_domain != parent_.scheduler_domain)
                return reject();
            Key key{failure.child.actor_slot, failure.child.generation};
            OresNativeTerminalReceipt receipt{};
            const bool has_scheduler_receipt =
                ores_native_actor_scheduler_terminal_receipt(
                    scheduler_, parent_, failure.child, &receipt) != 0;
            // An identity-mode parent callback must be backed by an EXACT
            // scheduler-minted PANIC envelope, not a forged host argument.
            // Legacy slot callbacks retain compatibility for older embedders.
            if (ores_native_actor_scheduler_uses_identity_callbacks(scheduler_)
                    && !has_scheduler_receipt)
                return reject();
            if (has_scheduler_receipt &&
                    (receipt.failure.kind != failure.kind
                     || receipt.failure.failure_code != failure.failure_code
                     || receipt.failure.correlation != failure.correlation))
                return reject();
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
                // Only this exact scheduler callback may durably prepare
                // terminal evidence; a copied ActorRef or queued envelope
                // must never poison the journal before real delivery.
                std::optional<DurableTerminalKey> durable_key;
                if (durable_journal_) {
                    OresNativeTerminalReceipt preflight{};
                    if (!has_scheduler_receipt
                        || !ores_native_actor_scheduler_probe_delivered_terminal_receipt(
                            scheduler_, parent_, failure.child, &preflight)) {
                        ++rejected_;
                        return false;
                    }
                    durable_key = durable_identity(receipt.failure, admitted);
                    prepare_durable_terminal(*durable_key, failure.child);
                }
                // Identity-mode callbacks also require the actual scheduler-minted
                // typed CONTROL carrier. A recorded-but-queued receipt or
                // copied parent ActorRef cannot authorize host delivery.
                // Claim original-parent liveness AND one-use host authority under
                // the scheduler mutex that serializes with physical STOP.
                // STOP winning first preserves RECORDED evidence for recovery;
                // a claim winning first remains an in-flight host effect.
                if (has_scheduler_receipt &&
                    !(ores_native_actor_scheduler_uses_identity_callbacks(scheduler_)
                        ? ores_native_actor_scheduler_claim_delivered_terminal_receipt(
                            scheduler_, parent_, failure.child, &receipt)
                        : ores_native_actor_scheduler_claim_live_parent_terminal_receipt(
                            scheduler_, parent_, failure.child, &receipt))) {
                    ++rejected_;
                    return false;
                }
                if (durable_key)
                    (void)durable_journal_->claim(*durable_key);
                // Durable claim is fsync'd BEFORE supervisor CONTROL
                // enqueue. An ambiguous host exception is never replayed.
                // The scheduler guarantees physical DONE, and the exact
                // receipt remains CLAIMED on all subsequent exceptions.
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
            if (has_scheduler_receipt &&
                !ores_native_actor_scheduler_ack_terminal_receipt(
                    scheduler_, receipt))
                return reject(); // external admission may have succeeded: no replay
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++accepted_;
            }
            return true;
        } catch (...) {
            return reject(); // process counts its own failed enqueue
        }
    }

public:
    /**
     * Recover a terminal PANIC rejected because the OLD parent stopped.
     * The original bridge and SupervisorProcess must remain owned by the
     * host; the recovered parent is a newer, scheduler-published generation
     * at the SAME physical supervisor slot, running identity-mode callbacks.
     *
     * The original scheduler receipt, not caller-provided failure payload,
     * is the sole authority. The first claim is irreversible: if process
     * enqueue fails ambiguously, the receipt remains CLAIMED and must NOT be
     * replayed. Only a successful enqueue may be ACKNOWLEDGED.
     *
     * Host-only, in-memory reconciliation. NOT process-crash durability or
     * source-language RESTART. Physical PANIC actor recycling stays fenced.
     */
    bool reconcile_quarantined_terminal(
            OresNativeActorRef recovered_parent,
            OresNativeActorRef failed_child) {
        if (!valid(recovered_parent)
                || recovered_parent.scheduler_domain != parent_.scheduler_domain
                || recovered_parent.actor_slot != parent_.actor_slot
                || recovered_parent.generation <= parent_.generation
                || !ores_native_actor_scheduler_uses_identity_callbacks(scheduler_)
                || ores_native_actor_scheduler_poll_published(scheduler_, parent_)
                || !ores_native_actor_scheduler_poll_published(
                       scheduler_, recovered_parent)
                || !process_->healthy())
            throw std::logic_error("no authorized recovered supervisor CONTROL owner");
        OresNativeTerminalReceipt receipt{};
        if (!ores_native_actor_scheduler_terminal_receipt(
                scheduler_, parent_, failed_child, &receipt)
                || receipt.failure.kind != ORES_NATIVE_CONTROL_PANIC)
            throw std::logic_error("no original scheduler PANIC evidence");
        SupervisorChildRegistry::Snapshot admitted;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const Key key{failed_child.actor_slot, failed_child.generation};
            const auto found = registered_.find(key);
            if (found == registered_.end())
                throw std::logic_error("terminal failure already consumed");
            if (!ores_native_actor_scheduler_poll_done(scheduler_, failed_child))
                throw std::logic_error("failed child is not physically DONE");
            admitted = found->second;
            const auto current = child_registry_->snapshot(admitted.child);
            if (current.phase != SupervisorChildRegistry::Phase::active
                    || current.incarnation != admitted.incarnation
                    || current.actor.scheduler_domain != failed_child.scheduler_domain
                    || current.actor.actor_slot != failed_child.actor_slot
                    || current.actor.generation != failed_child.generation)
                throw std::logic_error("stale logical child PANIC authority");
            std::optional<DurableTerminalKey> durable_key;
            if (durable_journal_) {
                durable_key = durable_identity(receipt.failure, admitted);
                prepare_durable_terminal(*durable_key, failed_child);
            }
            if (!ores_native_actor_scheduler_claim_recovered_parent_terminal_receipt(
                    scheduler_, parent_, recovered_parent, failed_child,
                    &receipt))
                throw std::logic_error(
                    "recovered parent stopped or terminal host effect already claimed");
            if (durable_key)
                (void)durable_journal_->claim(*durable_key);
            child_registry_->finalized(
                admitted.child, failed_child, admitted.incarnation);
            registered_.erase(found);
        }
        const auto& failure = receipt.failure;
        SupervisorFailureNotice copied{
            process_->supervisor_id(), process_->runtime_id(),
            failure.child.actor_slot, failure.child.generation,
            recovered_parent.actor_slot, "reconciled native terminal failure",
            failure.kind, failure.failure_code, failure.correlation,
            admitted.child.logical_child_id, admitted.incarnation
        };
        process_->accept(copied); // ambiguous exception => CLAIMED, never replay
        if (!ores_native_actor_scheduler_ack_terminal_receipt(
                scheduler_, receipt))
            throw std::logic_error("reconciled effect accepted but not acknowledged");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++accepted_;
        }
        return true;
    }

public:
    /**
     * Complete a previously CLAIMED terminal receipt after an independently
     * verified host effect. This is the ONLY acceptable outcome for lost
     * acknowledgment: DO NOT reenter process_->accept or replay policy.
     *
     * The verifier must consult an authoritative host commit receipt
     * bound to the exact original parent/child/failure/nonce; a timeout,
     * healthy process or invocation attempt alone is NOT proof. It executes
     * outside scheduler and bridge locks. False leaves the receipt CLAIMED.
     * This process-local API does not survive loss of the scheduler ledger.
     */
    bool reconcile_claimed_terminal_ack(
            OresNativeActorRef original_parent,
            OresNativeActorRef failed_child,
            const std::function<bool(const OresNativeTerminalReceipt&)>&
                verify_committed_effect) {
        if (!verify_committed_effect
                || original_parent.scheduler_domain != parent_.scheduler_domain
                || original_parent.actor_slot != parent_.actor_slot
                || original_parent.generation != parent_.generation)
            throw std::logic_error("unauthorized original PANIC receipt verifier");
        OresNativeTerminalReceipt receipt{};
        if (!ores_native_actor_scheduler_terminal_receipt(
                scheduler_, original_parent, failed_child, &receipt)
                || receipt.status != ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                || !receipt.nonce)
            throw std::logic_error("no unresolved claimed terminal effect");
        if (!verify_committed_effect(receipt)) return false;
        if (!ores_native_actor_scheduler_ack_terminal_receipt(scheduler_, receipt))
            throw std::logic_error("terminal effect claim changed while verified");
        return true;
    }


public:
    /**
     * Trusted host reconciliation of a durable terminal effect. The journal's
     * fsync'd external proof must precede the final host ACK, even when the
     * scheduler has already ACKed CONTROL enqueue (not effect execution).
     * Never reenters SupervisorProcess::accept or reruns its policy/sink.
     *
     * A crash-stranded effect_proven record can be ACKed without rerunning
     * its verifier. The host may instead reopen the journal after a process
     * crash and ACK directly, since the in-memory scheduler no longer exists.
     * A CLAIMED record is never sufficient evidence by itself.
     */
    bool reconcile_durable_terminal_effect(
            const ExternalEffectProof& proof,
            const std::function<bool(const ExternalEffectProof&)>&
                verify_committed_effect) {
        if (!durable_journal_)
            throw std::logic_error("durable journal not installed");
        const auto& w = proof.key.words;
        if (!proof.key.valid() || !proof.nonce
                || w[0] != process_->supervisor_id()
                || w[1] != process_->runtime_id()
                || w[2] != parent_.scheduler_domain
                || w[3] != parent_.actor_slot
                || w[4] != parent_.generation
                || w[5] != parent_.scheduler_domain)
            throw std::logic_error("foreign durable terminal receipt identity");

        const OresNativeActorRef child{w[5], w[6], w[7]};
        OresNativeTerminalReceipt receipt{};
        if (!ores_native_actor_scheduler_terminal_receipt(
                scheduler_, parent_, child, &receipt)
                || (receipt.status != ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
                    && receipt.status != ORES_NATIVE_TERMINAL_RECEIPT_ACKNOWLEDGED)
                || receipt.failure.kind != w[8]
                || receipt.failure.failure_code != w[9]
                || receipt.failure.correlation != w[10])
            throw std::logic_error("no matching scheduler terminal claim");

        const auto before = durable_journal_->inspect(proof.key);
        if (before.nonce != proof.nonce)
            throw std::logic_error("stale durable terminal nonce");
        if (before.phase == DurableTerminalPhase::claimed) {
            if (!durable_journal_->prove_external_effect(
                    proof, verify_committed_effect)) return false;
        } else if (before.phase != DurableTerminalPhase::effect_proven
                   && before.phase != DurableTerminalPhase::acknowledged) {
            throw std::logic_error("durable terminal effect is not proven");
        }
        const auto proven = durable_journal_->inspect(proof.key);
        if (proven.nonce != proof.nonce
                || proven.external_evidence_sha256 != proof.evidence_sha256)
            throw std::logic_error("durable external proof changed");
        if (proven.phase == DurableTerminalPhase::acknowledged) {
            if (receipt.status == ORES_NATIVE_TERMINAL_RECEIPT_ACKNOWLEDGED)
                return false; // completed duplicate; no effect replay
        } else {
            durable_journal_->acknowledge(proof); // fsync before scheduler ACK
        }
        if (receipt.status == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
            && !ores_native_actor_scheduler_ack_terminal_receipt(
                   scheduler_, receipt))
            throw std::logic_error(
                "durable terminal ACK persisted but scheduler receipt changed");
        return true;
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

    /**
     * One-shot, exception-safe native RESTART after a failed unpublished
     * candidate. The old physical candidate must already be DONE. We first
     * retire its original logical permit, then recycle the physical slot,
     * acquire a NEW logical permit, and publish via the existing scheduler
     * admission lease and CONTROL close fence.
     *
     * CRITICAL: no raw newly-live ActorRef escapes this operation. If closing
     * CONTROL, staging, admission or a trusted injection hook fails AFTER
     * recycling, this function physically stops and waits for exact DONE
     * before rolling back the new logical reservation. An inability to prove
     * cleanup is fatal for this trusted native host (never ignore an orphan).
     *
     * The optional hook is only for trusted-host fault injection/tests. It
     * hooks run outside publication/registry locks at the two handoff seams;
     * they must not initiate guest work on the hidden actor.
     */
    SupervisorChildRegistry::Snapshot abort_recycle_and_admit_verified(
            SupervisorChildRegistry::RestartReservation previous,
            OresNativeActorRef discarded,
            const std::function<void()>& after_recycle = {},
            const std::function<void()>& before_admission = {}) {
        const auto restored = abort_cleaned_replacement(previous, discarded);
        if (restored.phase != SupervisorChildRegistry::Phase::finalized)
            throw std::logic_error("restart abort did not restore FINALIZED");

        // Take CONTROL's decision/close gate before the scheduler's physical
        // publication lock; no bridge routing mutex is held across recycling.
        const auto hidden = process_->with_healthy_restart_transaction([&] {
            if (!child_registry_->restart_admission_open())
                throw std::logic_error(
                    "closed supervisor cannot recycle a live hidden candidate");
            const auto actor = ores_native_actor_scheduler_recycle_unpublished_done(
                scheduler_, discarded);
            if (!actor.scheduler_domain)
                throw std::logic_error(
                    "physically cleaned replacement could not recycle safely");
            return actor;
        });
        // From this point until successful publication THIS stack frame owns
        // the hidden actor. Do not allow a callback or exception to abandon it.
        std::optional<SupervisorChildRegistry::RestartReservation> next;
        try {
            if (after_recycle) after_recycle();
            next = child_registry_->begin_restart(
                previous.child, previous.failed_actor, previous.incarnation);
            if (before_admission) before_admission();
            const auto committed = admit_verified_replacement(*next, hidden);
            return committed; // scheduler publication is complete and fenced
        } catch (...) {
            const auto original = std::current_exception();
            // No scheduler lease survives admit_verified_replacement's scope.
            // Stop is idempotent with an already-completed actor; both cases
            // require the authoritative exact-generation DONE check.
            if (!ores_native_actor_scheduler_poll_done(scheduler_, hidden)) {
                (void) ores_native_actor_scheduler_stop(scheduler_, hidden);
                if (!ores_native_actor_scheduler_wait_done(scheduler_, hidden)
                    || !ores_native_actor_scheduler_poll_done(scheduler_, hidden))
                    std::terminate(); // never return while an orphan is live
            }
            if (next) {
                // If staging succeeded, prove cleanup before logical abort.
                // If admission failed before staging, no candidate exists and
                // abort_restart consumes the otherwise-stranded new permit.
                try {
                    child_registry_->acknowledge_replacement_cleanup(
                        *next, hidden, [&] {
                            return ores_native_actor_scheduler_poll_done(
                                scheduler_, hidden) != 0;
                        });
                } catch (const std::logic_error&) {
                    // Accept only the no-candidate path: abort_restart itself
                    // still rejects a live or uncleaned staged replacement.
                }
                try {
                    (void) child_registry_->abort_restart(*next);
                } catch (...) {
                    std::terminate(); // preserve physical/logical ownership
                }
            }
            std::rethrow_exception(original);
        }
    }

    /** Terminal supervisor teardown fences any in-flight RESTART commit. */
    void fence_restart_publication() {
        // Closure fences all future registry publication. A concurrently
        // returned hidden candidate still belongs to its trusted host:
        // the caller MUST stop it if acquiring the next ticket fails.
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
    DurableTerminalKey durable_identity(
            const OresNativeActorTerminalFailure& failure,
            const SupervisorChildRegistry::Snapshot& admitted) const {
        return DurableTerminalKey::from(
            process_->supervisor_id(), process_->runtime_id(),
            parent_.scheduler_domain, parent_.actor_slot, parent_.generation,
            failure.child.scheduler_domain, failure.child.actor_slot,
            failure.child.generation, failure.kind,
            failure.failure_code, failure.correlation,
            admitted.child.logical_child_id, admitted.incarnation);
    }
    void prepare_durable_terminal(
            const DurableTerminalKey& identity,
            OresNativeActorRef physical_child) {
        if (!durable_journal_) return;
        auto state = durable_journal_->find(identity);
        if (!state) {
            durable_journal_->record(identity);
            state = durable_journal_->find(identity);
        }
        if (state->phase == DurableTerminalPhase::recorded) {
            if (!durable_journal_->record_physical_done(identity, [&] {
                    return ores_native_actor_scheduler_poll_done(
                        scheduler_, physical_child) != 0;
                }))
                throw std::logic_error(
                    "physical DONE not verified before durable terminal claim");
        } else if (state->phase != DurableTerminalPhase::physical_done) {
            throw std::logic_error(
                "durable terminal receipt already claimed or acknowledged");
        }
    }
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
    const std::shared_ptr<NativeTerminalReceiptJournal> durable_journal_;
    mutable std::mutex mutex_;
    std::unordered_map<Key, SupervisorChildRegistry::Snapshot, KeyHash> registered_;
    std::size_t accepted_ = 0;
    std::size_t rejected_ = 0;
};

} // namespace oreslang::runtime
