#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SupervisorChildRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace oreslang::runtime {

/**
 * Trusted-host, process-local physical STOP evidence.
 *
 * This ledger observes the native actor scheduler's physical DONE barrier and
 * captures an immutable, exact-incarnation receipt separate from a source
 * supervisor's asynchronous/exceptional host acknowledgment.
 *
 * Scope is deliberately narrow: physical actor termination ONLY. It is not
 * evidence that an arbitrary network/database/third-party STOP side effect
 * happened. Host callers must not expose this API to guest code, replace
 * physical scheduler checks with a Boolean, or call this after scheduler
 * destruction. The ledger is not durable across process crashes.
 */
class NativePhysicalStopEvidence final {
public:
    using StopReceipt = SupervisorChildRegistry::StopReceipt;
    using Status = SupervisorChildRegistry::StopReceiptStatus;
    struct Attestation {
        std::uint64_t evidence_sequence;
        std::uint64_t supervisor_id;
        std::uint64_t runtime_id;
        std::uint64_t logical_child_id;
        OresNativeActorRef actor;
        std::uint64_t incarnation;
        std::uint64_t control_turn;
    };

    NativePhysicalStopEvidence(
            OresNativeActorScheduler* scheduler,
            std::shared_ptr<SupervisorChildRegistry> registry)
        : scheduler_(scheduler),
          registry_(std::move(registry)) {
        if (!scheduler_ || !registry_
            || !registry_->supervisor_id() || !registry_->runtime_id())
            throw std::invalid_argument("invalid native STOP evidence owner");
    }

    /**
     * Observe actual scheduler DONE and record one host-owned attestation.
     * Call from the trusted physical lifecycle adapter, after DONE is true,
     * and before returning a STOP acknowledgment to guest CONTROL.
     */
    Attestation observe_physical_done(const StopReceipt& requested) {
        // The owner must call close() before destroying its scheduler.
        // Keep scheduler use under the same mutex as close(), so a concurrent
        // observer cannot race the final lifetime revocation.
        std::lock_guard<std::mutex> guard(mutex_);
        if (!scheduler_)
            throw std::logic_error("native STOP evidence ledger closed");
        if (!authoritative(requested))
            throw std::logic_error("foreign, forged or unclaimed native STOP receipt");
        if (!ores_native_actor_scheduler_poll_done(
                scheduler_, requested.failed_actor))
            throw std::logic_error("native STOP has no physical DONE barrier");
        const auto key = binding(requested);
        if (observed_.count(key))
            throw std::logic_error("physical STOP receipt already recorded");
        if (observed_.size() >= max_evidence
                || sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::logic_error("physical STOP evidence ledger exhausted");
        const Attestation result{
            sequence_ + 1,
            requested.child.supervisor_id, requested.child.runtime_id,
            requested.child.logical_child_id,
            requested.failed_actor,
            requested.incarnation, requested.turn_generation};
        observed_.emplace(key, result);
        ++sequence_;
        return result;
    }

    /**
     * Independent read-only reconciliation after the original callback or
     * acknowledgment was lost. Never reexecutes a STOP effect.
     */
    bool confirms_physical_done(const StopReceipt& requested) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!scheduler_ || !authoritative(requested)) return false;
        const auto it = observed_.find(binding(requested));
        if (it == observed_.end()) return false;
        // Even a valid old ledger entry must not authorize a still-live actor.
        return ores_native_actor_scheduler_poll_done(
            scheduler_, requested.failed_actor) != 0;
    }

    /**
     * Reclaim only independently attested, already ACKNOWLEDGED evidence.
     * This must precede registry child retirement; unresolved STOPs remain
     * quarantined and cannot be silently discarded to regain capacity.
     */
    bool forget_acknowledged(const StopReceipt& supplied) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!scheduler_) return false;
        try {
            const auto current = registry_->stop_receipt(supplied.child);
            if (current.status != Status::acknowledged
                || !current.host_invoked
                || binding(current) != binding(supplied))
                return false;
            return observed_.erase(binding(current)) == 1;
        } catch (...) {
            return false;
        }
    }

    /**
     * The trusted scheduler owner MUST close this adapter and allow close()
     * to finish before calling scheduler try_destroy(). All future reads
     * fail closed. The ledger is volatile, so close() cannot recover a crash.
     */
    void close() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        scheduler_ = nullptr;
        observed_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return observed_.size();
    }

private:
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t, std::uint64_t,
                           std::uint64_t>;
    static constexpr std::size_t max_evidence = 65536;

    bool valid(const StopReceipt& r) const noexcept {
        return r.host_invoked
            && (r.status == Status::pending || r.status == Status::uncertain)
            && r.child.supervisor_id == registry_->supervisor_id()
            && r.child.runtime_id == registry_->runtime_id()
            && r.child.logical_child_id != 0
            && r.failed_actor.scheduler_domain == registry_->runtime_id()
            // Scheduler slot zero is a valid actor address (often parent).
            // Domain and generation, not slot truthiness, establish validity.
            && r.failed_actor.generation != 0
            && r.incarnation != 0 && r.turn_generation != 0;
    }

    bool authoritative(const StopReceipt& supplied) const {
        if (!valid(supplied)) return false;
        try {
            // Host registry, not a copied structure from the callback, is
            // the authority for STOP reservation and invocation status.
            const auto current = registry_->stop_receipt(supplied.child);
            return current.child.supervisor_id == supplied.child.supervisor_id
                && current.child.runtime_id == supplied.child.runtime_id
                && current.child.logical_child_id == supplied.child.logical_child_id
                && current.failed_actor.scheduler_domain
                    == supplied.failed_actor.scheduler_domain
                && current.failed_actor.actor_slot == supplied.failed_actor.actor_slot
                && current.failed_actor.generation == supplied.failed_actor.generation
                && current.incarnation == supplied.incarnation
                && current.turn_generation == supplied.turn_generation
                && current.host_invoked
                && (current.status == Status::pending
                    || current.status == Status::uncertain);
        } catch (...) {
            return false;
        }
    }

    static Key binding(const StopReceipt& r) noexcept {
        return {r.child.supervisor_id, r.child.runtime_id,
                r.child.logical_child_id, r.failed_actor.actor_slot,
                r.failed_actor.generation, r.incarnation, r.turn_generation};
    }

    OresNativeActorScheduler* scheduler_;
    const std::shared_ptr<SupervisorChildRegistry> registry_;
    mutable std::mutex mutex_;
    std::map<Key, Attestation> observed_;
    std::uint64_t sequence_ = 0;
};

} // namespace oreslang::runtime
