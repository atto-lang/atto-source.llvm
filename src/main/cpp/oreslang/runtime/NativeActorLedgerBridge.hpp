#pragma once

#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/BoundedMpscQueue.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

// Host-only binding to the EXISTING NativeActorScheduler C11 actor carrier.
// Caller MUST route all scheduler on_message/on_control callbacks here.
// The actor-owned ledger and payload slots are touched only under the native
// generation-checked exclusive actor execution claim, not from clock threads.
// Notification ingress is bounded MPSC and stores opaque IDs, not guest refs.
// No clock source, source await lowering or hard-RT execution proof is provided.
template<std::size_t EventSlots, std::size_t NoticeSlots = 8>
class NativeActorLedgerBridge final {
    static_assert(EventSlots >= 2, "reserve a slot for supervisor CONTROL");
public:
    using Ledger = ActorExecutionLedger<EventSlots>;
    using Id = typename Ledger::EventId;
    using Kind = typename Ledger::Kind;
    using Status = typename Ledger::Status;
    enum class Decision : std::uint8_t { Finish, Suspend };
    enum class Submit : std::uint8_t {
        Accepted, Full, Contended, SchedulerRejected, Closed
    };
    enum class Notify : std::uint8_t {
        Signaled, Coalesced, RetryRequired, Full, Invalid, Closed
    };
    using Handler = Decision (*)(void*, Id, Kind, std::uint64_t) noexcept;

    NativeActorLedgerBridge(OresNativeActorScheduler* scheduler,
                            OresNativeActorRef actor, Handler on_event,
                            void* handler_context) noexcept
        : scheduler_(scheduler), actor_(actor),
          ledger_(actor.actor_slot + 1, actor.generation),
          on_event_(on_event), handler_context_(handler_context) {}
    NativeActorLedgerBridge(const NativeActorLedgerBridge&) = delete;
    NativeActorLedgerBridge& operator=(const NativeActorLedgerBridge&) = delete;

    // Reserve actor-event credit BEFORE accepting a native scheduler message.
    // EventSlots-1 leaves at least one logical slot for mandatory supervisor
    // CONTROL; the native scheduler must still reserve its own CONTROL slot.
    Submit send(std::uint64_t payload) noexcept {
        if (closed_.load(std::memory_order_acquire)) return Submit::Closed;
        const auto credit = reserve_credit();
        if (credit != Submit::Accepted) return credit;
        if (ores_native_actor_scheduler_try_send(scheduler_, actor_, payload)
                != ORES_NATIVE_SEND_ACCEPTED) {
            credits_.fetch_sub(1, std::memory_order_acq_rel);
            return Submit::SchedulerRejected;
        }
        return Submit::Accepted;
    }

    // Actor-carrier-only timer/continuation registration. A clock producer
    // cannot directly write into the actor ledger or borrow guest captures.
    Status admit_timer(Id& out) noexcept {
        if (!authorized() || closed_.load(std::memory_order_acquire))
            return Status::Unauthorized;
        if (reserve_credit() != Submit::Accepted) return Status::Full;
        const auto status = ledger_.admit(Kind::Timer, out);
        if (status != Status::Ok) {
            credits_.fetch_sub(1, std::memory_order_acq_rel);
            return status;
        }
        if (!save(out, Kind::Timer, 0)) {
            (void)ledger_.cancel(out);
            credits_.fetch_sub(1, std::memory_order_acq_rel);
            return Status::Full;
        }
        return Status::Ok;
    }

    // Scheduler callback, never directly from clock/producer threads.
    void on_message(std::uint64_t payload) noexcept {
        if (!authorized()) { failures_.fetch_add(1); return; }
        Id id{};
        if (ledger_.admit(Kind::Message, id) != Status::Ok ||
                !save(id, Kind::Message, payload)) {
            failures_.fetch_add(1);
            return; // already-admitted work remains charged, never hidden
        }
        messages_arrived_.fetch_add(1, std::memory_order_release);
        run_ready();
    }

    // Called on actual actor CONTROL carrier. Only token readiness is copied.
    bool on_control(std::uint64_t payload) noexcept {
        if (!authorized()) { failures_.fetch_add(1); return false; }
        // This reserved scheduler CONTROL lane may also carry supervisory
        // events; only the caller can distinguish and route those envelopes.
        if (payload != control_payload_) return false;
        Id id{};
        std::size_t examined = 0;
        while (examined++ < NoticeSlots && notices_.try_pop(id)) {
            const auto state = ledger_.notify_ready(id);
            if (state == Status::Stale || state == Status::AlreadyReady ||
                state == Status::Closed) stale_notices_.fetch_add(1);
            else if (state != Status::Ok) failures_.fetch_add(1);
        }
        // Coalesced notification handshake: clear first, then recheck the
        // head; a producer racing the clear either publishes a fresh signal
        // or is observed by this recheck. Failed publish remains retryable.
        signaled_.store(false, std::memory_order_release);
        if (notices_.has_ready() && kick() == Notify::RetryRequired)
            retry_required_.store(true, std::memory_order_release);
        run_ready();
        return true;
    }

    // Used only for an already-admitted NATIVE scheduler supervisor CONTROL
    // callback, on its authenticated actor carrier. Distinct from an ordinary
    // timer wake (which must not bypass a suspended serialized logical turn).
    bool on_supervisor_control(std::uint64_t payload) noexcept {
        if (!authorized()) { failures_.fetch_add(1); return false; }
        Id id{};
        if (ledger_.admit(Kind::Control, id) != Status::Ok ||
            !save(id, Kind::Control, payload, false)) {
            failures_.fetch_add(1); return false;
        }
        run_ready();
        return true;
    }

    // An external timer/future producer stages opaque identity only. Full
    // queue is a typed rejection; publish_control failure is a retry-needed
    // state, NEVER successful delivery. Caller must arrange retry_kick().
    Notify notify_ready(Id id) noexcept {
        if (!id.actor_id || id.actor_id != actor_.actor_slot + 1 ||
            id.actor_generation != actor_.generation || !id.sequence)
            return Notify::Invalid;
        if (closed_.load(std::memory_order_acquire)) return Notify::Closed;
        if (!notices_.try_push(id)) return Notify::Full;
        return kick();
    }
    Notify retry_kick() noexcept {
        if (closed_.load(std::memory_order_acquire)) return Notify::Closed;
        if (!notices_.has_ready()) return Notify::Coalesced;
        return kick();
    }

    // Must be called by supervisor AFTER native wait_done / physical carrier
    // quiescence, with all asynchronous inbox producers externally fenced.
    // Without that external proof, this function MUST NOT be called.
    bool retire_after_native_done() noexcept {
        if (!scheduler_ || !ores_native_actor_scheduler_poll_done(scheduler_, actor_))
            return false;
        closed_.store(true, std::memory_order_release);
        (void)ledger_.stop();
        Id discarded{};
        while (notices_.try_pop(discarded)) {}
        credits_.store(0, std::memory_order_release);
        return ledger_.physically_retired();
    }

    bool actor_authorized() const noexcept { return authorized(); }
    std::uint64_t failures() const noexcept { return failures_.load(); }
    std::uint64_t stale_notices() const noexcept { return stale_notices_.load(); }
    std::uint64_t messages_arrived() const noexcept { return messages_arrived_.load(); }
    bool retry_needed() const noexcept { return retry_required_.load(); }
    std::size_t reserved() const noexcept { return credits_.load(); }
    const Ledger& ledger_for_current_actor() const noexcept { return ledger_; }
private:
    struct Slot { Id id{}; Kind kind = Kind::Message; std::uint64_t payload = 0; bool live = false; bool charged = false; };
    bool authorized() const noexcept {
        return scheduler_ && actor_.scheduler_domain && actor_.generation &&
            ores_native_actor_scheduler_is_current_actor(scheduler_, actor_) != 0;
    }
    Submit reserve_credit() noexcept {
        auto current = credits_.load(std::memory_order_acquire);
        if (current >= EventSlots - 1) return Submit::Full;
        // No unbounded CAS loop on a time-critical entry path.
        if (!credits_.compare_exchange_strong(current, current + 1,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return Submit::Contended;
        if (closed_.load(std::memory_order_acquire)) {
            credits_.fetch_sub(1, std::memory_order_acq_rel);
            return Submit::Closed;
        }
        return Submit::Accepted;
    }
    bool save(Id id, Kind kind, std::uint64_t payload, bool charged = true) noexcept {
        for (auto& slot : slots_) {
            if (slot.live) continue;
            slot = Slot{id, kind, payload, true, charged};
            return true;
        }
        return false;
    }
    void run_ready() noexcept {
        for (std::size_t i = 0; i < EventSlots; ++i) {
            Id chosen{};
            if (ledger_.select(chosen) != Status::Ok) return;
            Slot* entry = nullptr;
            for (auto& slot : slots_) {
                if (slot.live && slot.id.sequence == chosen.sequence) {
                    entry = &slot; break;
                }
            }
            if (!entry || ledger_.claim(chosen, authorized()) != Status::Ok) {
                failures_.fetch_add(1); return;
            }
            if (!on_event_) { failures_.fetch_add(1); return; }
            const auto outcome = on_event_(handler_context_, chosen,
                                           entry->kind, entry->payload);
            if (outcome == Decision::Suspend) {
                if (ledger_.suspend(chosen) != Status::Ok) failures_.fetch_add(1);
                return;
            }
            if (ledger_.finish(chosen) != Status::Ok) {
                failures_.fetch_add(1); return;
            }
            entry->live = false;
            if (entry->charged) credits_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    Notify kick() noexcept {
        bool expected = false;
        if (!signaled_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return Notify::Coalesced;
        if (!ores_native_actor_scheduler_publish_control(
                scheduler_, actor_, control_payload_)) {
            signaled_.store(false, std::memory_order_release);
            retry_required_.store(true, std::memory_order_release);
            return Notify::RetryRequired;
        }
        retry_required_.store(false, std::memory_order_release);
        return Notify::Signaled;
    }

    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef actor_;
    Ledger ledger_;
    Handler on_event_;
    void* handler_context_;
    std::array<Slot, EventSlots> slots_{};
    BoundedMpscQueue<Id, NoticeSlots> notices_{};
    std::atomic<std::size_t> credits_{0};
    std::atomic<bool> signaled_{false};
    std::atomic<bool> retry_required_{false};
    std::atomic<bool> closed_{false};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> stale_notices_{0};
    std::atomic<std::uint64_t> messages_arrived_{0};
    static constexpr std::uint64_t control_payload_ = 0x4f52455354494d45ull;
};
} // namespace oreslang::runtime
