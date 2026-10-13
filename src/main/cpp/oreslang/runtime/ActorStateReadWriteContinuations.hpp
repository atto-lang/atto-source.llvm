#pragma once

#include "oreslang/runtime/ActorStateReadWriteGate.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

/**
 * Trusted scheduler-side continuation registry for the actor-state RW gate.
 *
 * This is NOT a guest-call dispatcher. It never invokes actor code, creates
 * a carrier, blocks for admission or presumes a CONTROL mailbox receipt.
 * The scheduler polls notifications and acknowledges each only after its
 * bounded CONTROL path has accepted the notification. A failed enqueue is
 * retried by polling again. All guest execution remains single-flight.
 *
 * Lifecycle precondition: retire is called after guest turn quiescence and
 * live state-loan draining. A gate ticket alone cannot prove either.
 */
class ActorStateReadWriteContinuations final {
    // The bridge verifies the physical scheduler actor-lane before claiming
    // or completing a dispatched loan. No other component may invoke the
    // raw registry entrypoints that lack actor-turn provenance.
    friend class ActorStateReadWriteSchedulerBridge;
public:
    using Gate = ActorStateReadWriteGate;
    using Mode = Gate::Mode;
    static constexpr std::size_t max_outstanding_continuations =
            Gate::max_waiters + Gate::max_active_readers;
    enum class Status : std::uint8_t { Queued, Ready, Cancelled };
    struct Request {
        Status status;
        Mode mode;
        std::uint64_t incarnation;
    };
    struct Notification {
        std::uint64_t incarnation;
        std::uint64_t continuation_id;
        Status status;
    };

    ActorStateReadWriteContinuations() : incarnation_(mint_incarnation()) {}
    [[nodiscard]] std::uint64_t incarnation() const noexcept { return incarnation_; }
    ActorStateReadWriteContinuations(const ActorStateReadWriteContinuations&) = delete;
    ActorStateReadWriteContinuations& operator=(const ActorStateReadWriteContinuations&) = delete;

    [[nodiscard]] Request request(
            std::uint64_t incarnation, std::uint64_t continuation_id, Mode mode) {
        std::lock_guard<std::mutex> lock(mu_);
        validate_incarnation(incarnation);
        if (retired_) throw std::logic_error("actor state continuations retired");
        if (continuation_id == 0 || continuation_id <= issued_continuation_) {
            throw std::invalid_argument("continuation identity must increase monotonically");
        }
        // Includes cancelled-but-unacknowledged continuations, which no
        // longer consume gate tickets but still consume durable CONTROL space.
        if (entries_.size() >= max_outstanding_continuations) {
            throw std::overflow_error("actor state continuation registry exhausted");
        }
        // Prepare registry storage before obtaining an admission ticket.
        auto [it, inserted] = entries_.try_emplace(continuation_id);
        if (!inserted) throw std::logic_error("duplicate continuation");
        try {
            const auto granted = gate_.request(mode);
            it->second.token = granted.token;
            it->second.mode = mode;
            it->second.status = granted.admission == Gate::Admission::acquired
                    ? Status::Ready : Status::Queued;
            // A gate ticket is not scheduler authorization. Even a grant
            // obtained immediately must enter the bounded CONTROL channel
            // and receive its exact READY admission receipt before execution.
            // Otherwise an arbitrary host caller could bypass actor-lane
            // scheduling simply by requesting an uncontended gate.
            it->second.needs_ack = it->second.status == Status::Ready;
            issued_continuation_ = continuation_id;
            return {it->second.status, mode, incarnation_};
        } catch (...) {
            entries_.erase(it);
            throw;
        }
    }

    // Called ONLY from the trusted scheduler's verified actor callback,
    // not on the producer thread. The producer can acknowledge admission
    // after this callback starts; claim resolves that handoff race atomically.
    // One-shot dispatch claims prevent duplicate/replayed READY envelopes
    // from executing guest code twice.
private:
    [[nodiscard]] bool try_claim_ready(
            const Notification& wake, Mode expected_mode) {
        std::lock_guard<std::mutex> lock(mu_);
        if (retired_ || wake.incarnation != incarnation_
                || wake.status != Status::Ready) return false;
        auto it = entries_.find(wake.continuation_id);
        if (it == entries_.end() || it->second.status != Status::Ready
                || it->second.mode != expected_mode || it->second.dispatched
                || !gate_.holds(it->second.token, expected_mode)) return false;
        it->second.dispatched = true;
        it->second.needs_ack = false;
        return true;
    }

    // A backend may finish only a previously claimed actor-lane wake. This
    // is stronger than legacy complete() used in host-only gate tests.
    void complete_dispatched(
            std::uint64_t incarnation, std::uint64_t continuation_id) {
        complete_impl(incarnation, continuation_id, true);
    }

public:
    // Only native, explicitly opted-in test harnesses can synthesize
    // completion without receiving a scheduler-owned wake. Production builds
    // have no host completion entrypoint, including pre-dispatch READY tickets.
#if defined(ORESLANG_RW_INTERNAL_TEST_HOOKS)
    void complete_undispatched_for_test(
            std::uint64_t incarnation, std::uint64_t continuation_id) {
        complete_impl(incarnation, continuation_id, false);
    }
#endif

private:
    void complete_impl(std::uint64_t incarnation,
                       std::uint64_t continuation_id, bool require_dispatch) {
        std::lock_guard<std::mutex> lock(mu_);
        validate_incarnation(incarnation);
        auto it = require_entry(continuation_id);
        if (retired_ || it->second.status != Status::Ready
                || it->second.needs_ack
                || (require_dispatch && !it->second.dispatched)
                // Legacy host completion must never release a grant already
                // claimed by the actor-lane callback. Only the owning
                // scheduler scope may complete a dispatched continuation.
                || (!require_dispatch && it->second.dispatched)
                || !gate_.holds(it->second.token, it->second.mode)) {
            throw std::logic_error("continuation lacks an acknowledged state grant");
        }
        auto awakened = gate_.release(it->second.token);
        entries_.erase(it);
        mark_awakened(awakened);
    }

public:
    // A pending continuation may already own a gate ticket while its
    // READY notification awaits CONTROL admission. That loan is not yet
    // dispatched and may safely be revoked. An acknowledged READY grant
    // can be running guest code and MUST NOT be cancelled here.
    void cancel_pending(std::uint64_t incarnation, std::uint64_t continuation_id) {
        std::lock_guard<std::mutex> lock(mu_);
        validate_incarnation(incarnation);
        auto it = require_entry(continuation_id);
        if (retired_ || (it->second.status != Status::Queued
                && !(it->second.status == Status::Ready && it->second.needs_ack))) {
            throw std::logic_error("only undispatched actor state admission may be cancelled");
        }
        auto awakened = gate_.release(it->second.token);
        it->second.status = Status::Cancelled;
        it->second.needs_ack = true;
        mark_awakened(awakened);
    }

    /**
     * Quiescent retirement: revoke all remaining tickets and make their
     * cancellation durable until a CONTROL receipt is confirmed. Call only
     * after the runtime has stopped guest execution and drained state loans.
     */
    void retire() {
        std::lock_guard<std::mutex> lock(mu_);
        if (retired_) return;
        // A currently dispatched callback may still use state. A retiring
        // actor must first fence its guest lane and drain executing loans;
        // revoking live authority here could reclaim storage under a guest.
        // This check and completion share mu_, preventing a retirement race.
        for (const auto& [id, entry] : entries_) {
            (void)id;
            if (entry.dispatched) {
                throw std::logic_error(
                    "actor state retirement requires dispatched loan quiescence");
            }
        }
        // Gate retirement reserves cancellation storage before mutating.
        (void)gate_.retire();
        retired_ = true;
        for (auto& [id, entry] : entries_) {
            (void)id;
            entry.status = Status::Cancelled;
            entry.needs_ack = true;
        }
    }

    // Observing is not consuming: enqueue failure cannot lose a notification.
    // Bound each scheduler pump's snapshot to its actual CONTROL budget;
    // a full cancellation backlog must not allocate thousands of receipts
    // on every tick when only a handful can be admitted.
    [[nodiscard]] std::vector<Notification> pending_notifications(
            std::size_t limit = std::numeric_limits<std::size_t>::max()) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Notification> pending;
        pending.reserve(std::min(entries_.size(), limit));
        for (const auto& [id, entry] : entries_) {
            if (pending.size() == limit) break;
            if (entry.needs_ack) pending.push_back({incarnation_, id, entry.status});
        }
        return pending;
    }

    // Acknowledge only the exact status accepted by CONTROL admission.
    // A stale Ready receipt cannot acknowledge a later Cancelled status.
    [[nodiscard]] bool acknowledge(const Notification& accepted) {
        std::lock_guard<std::mutex> lock(mu_);
        if (accepted.incarnation != incarnation_) return false;
        auto it = entries_.find(accepted.continuation_id);
        if (it == entries_.end() || !it->second.needs_ack
                || it->second.status != accepted.status) return false;
        it->second.needs_ack = false;
        if (accepted.status == Status::Cancelled) entries_.erase(it);
        return true;
    }

    [[nodiscard]] bool admitted(
            std::uint64_t incarnation, std::uint64_t continuation_id, Mode mode) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (incarnation != incarnation_) return false;
        auto it = entries_.find(continuation_id);
        return !retired_ && it != entries_.end()
                && it->second.status == Status::Ready && !it->second.needs_ack
                && it->second.mode == mode
                && gate_.holds(it->second.token, mode);
    }

    [[nodiscard]] std::size_t waiting() const { return gate_.waiting(); }

private:
    static std::uint64_t mint_incarnation() {
        auto candidate = next_incarnation_.load(std::memory_order_relaxed);
        for (;;) {
            if (candidate == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("actor state incarnation space exhausted");
            }
            if (next_incarnation_.compare_exchange_weak(
                    candidate, candidate + 1, std::memory_order_relaxed)) {
                return candidate + 1;
            }
        }
    }

    void validate_incarnation(std::uint64_t expected) const {
        if (!expected || expected != incarnation_) {
            throw std::invalid_argument("stale or foreign actor state incarnation");
        }
    }

    struct Entry {
        Gate::Token token;
        Mode mode = Mode::Read;
        Status status = Status::Queued;
        bool needs_ack = false;
        bool dispatched = false;
    };
    using Entries = std::map<std::uint64_t, Entry>;
    Entries::iterator require_entry(std::uint64_t id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            throw std::invalid_argument("unknown actor state continuation");
        }
        return it;
    }
    void mark_awakened(const std::vector<Gate::Token>& awakened) {
        for (const auto& ticket : awakened) {
            bool found = false;
            for (auto& [id, entry] : entries_) {
                (void)id;
                if (entry.token == ticket && entry.status == Status::Queued) {
                    entry.status = Status::Ready;
                    entry.needs_ack = true;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::logic_error("unregistered actor state ticket");
        }
    }

    inline static std::atomic<std::uint64_t> next_incarnation_{0};
    const std::uint64_t incarnation_;
    mutable std::mutex mu_;
    Gate gate_;
    Entries entries_;
    std::uint64_t issued_continuation_ = 0;
    bool retired_ = false;
};

} // namespace oreslang::runtime
