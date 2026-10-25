#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

/**
 * Host-only bounded async mailbox-credit linearization.
 *
 * This primitive owns no guest payload and is not source send_async lowering.
 * The caller must freeze/copy payload ownership and retain the target's
 * physical resource loan before registering a waiter. This class supplies the
 * missing race-free accounting boundary between:
 *   - synchronous fail-fast USER credit,
 *   - bounded FIFO producer waiters,
 *   - scheduler-owned generation-fenced wake escrow,
 *   - cancellation before transfer only,
 *   - target STOP/seal failure wake,
 *   - irrevocable credit transfer.
 *
 * All storage is allocated at construction. Registration/release/cancel do not
 * grow a container. No guest callback executes under this mutex: scheduler
 * wake APIs only mutate/pre-enqueue trusted wake state.
 */
class NativeAsyncMailboxAdmission final {
public:
    struct Handle {
        std::size_t slot = static_cast<std::size_t>(-1);
        std::uint64_t generation = 0;
    };
    enum class Registration {
        Waiting,
        Transferred,
        Failed,
        Rejected
    };

    NativeAsyncMailboxAdmission(
            std::size_t max_frames,
            std::size_t max_bytes,
            std::size_t max_waiters,
            std::size_t max_pending_bytes)
        : max_frames_(max_frames),
          max_bytes_(max_bytes),
          max_waiters_(max_waiters),
          max_pending_bytes_(max_pending_bytes),
          slots_(max_waiters),
          fifo_(max_waiters) {
        if (!max_frames || !max_bytes || !max_waiters || !max_pending_bytes)
            throw std::invalid_argument("positive async mailbox limits required");
    }

    NativeAsyncMailboxAdmission(const NativeAsyncMailboxAdmission&) = delete;
    NativeAsyncMailboxAdmission& operator=(const NativeAsyncMailboxAdmission&) = delete;

    ~NativeAsyncMailboxAdmission() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (used_frames_ != 0 || used_bytes_ != 0
                || active_descriptors_ != 0 || pending_waiters_ != 0
                || pending_bytes_ != 0)
            std::terminate();
        for (const Slot& entry : slots_) {
            if (entry.state == State::Preparing
                    || entry.state == State::Waiting
                    || entry.state == State::Transferred
                    || entry.state == State::Publishing)
                std::terminate();
        }
    }

    /**
     * Reserve one target-side producer descriptor BEFORE graph validation,
     * byte estimation, freezing/copying or wake reservation. This mirrors the
     * Java AsyncSendAdmissionLoan: the advertised producer cap covers the
     * entire pre-publication cohort rather than only already-frozen waiters.
     */
    bool begin_preparation(Handle* out) {
        if (out) *out = {};
        std::lock_guard<std::mutex> guard(mutex_);
        if (sealed_ || active_descriptors_ >= max_waiters_) return false;
        const std::size_t slot = find_empty_slot_locked();
        if (slot == max_waiters_) return false;
        Slot& entry = slots_[slot];
        if (entry.generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++entry.generation;
        if (!entry.generation) std::terminate();
        entry.state = State::Preparing;
        entry.bytes = 0;
        entry.scheduler = nullptr;
        entry.wake = {};
        ++active_descriptors_;
        if (out) *out = Handle{slot, entry.generation};
        return true;
    }

    bool cancel_preparation(Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Preparing) return false;
        if (!active_descriptors_) std::terminate();
        --active_descriptors_;
        entry->state = State::Cancelled;
        return true;
    }

    // Synchronous send remains fail-fast. Existing async waiters have priority
    // over a racing synchronous sender even if bytes happen to be free.
    bool try_acquire(std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("mailbox byte charge must be positive");
        std::lock_guard<std::mutex> guard(mutex_);
        if (sealed_ || pending_waiters_ != 0 || !fits_locked(bytes))
            return false;
        ++used_frames_;
        used_bytes_ += bytes;
        return true;
    }

    /**
     * Register a producer after its wake ticket has been RESERVED.
     *
     * Rejected means this object did not take responsibility for the ticket.
     * Waiting/Transferred/Failed consume the registration attempt. Failed has
     * already converted the reserved continuation to FAILED (or retired it if
     * the producer stopped), so the caller must release retained payload/loan.
     */
    Registration register_prepared(
            Handle handle,
            std::size_t bytes,
            OresNativeActorScheduler* scheduler,
            OresNativeWakeTicket wake) {
        if (!bytes || !scheduler || !wake.slot_generation
                || !wake.descriptor_generation)
            return Registration::Rejected;

        std::unique_lock<std::mutex> guard(mutex_);
        Slot* entry_ptr = matching_locked(handle);
        if (!entry_ptr || entry_ptr->state != State::Preparing)
            return Registration::Rejected;
        Slot& entry = *entry_ptr;
        if (sealed_ || bytes > max_bytes_
                || pending_waiters_ >= max_waiters_
                || bytes > max_pending_bytes_ - pending_bytes_)
            return Registration::Rejected;

        compact_fifo_locked();
        entry.state = State::Waiting;
        entry.bytes = bytes;
        entry.scheduler = scheduler;
        entry.wake = wake;
        pending_bytes_ += bytes;
        ++pending_waiters_;

        // No earlier waiter may be bypassed. If this is the only waiter and a
        // full credit is available now, transfer immediately under the same
        // accounting mutex that synchronous admission uses.
        if (pending_waiters_ == 1 && fifo_entries_ == 0 && fits_locked(bytes)) {
            if (transfer_locked(entry)) {
                --pending_waiters_;
                ++used_frames_;
                used_bytes_ += bytes;
                entry.state = State::Transferred;
                return Registration::Transferred;
            }
            OresNativeActorScheduler* failed_scheduler = entry.scheduler;
            const OresNativeWakeTicket failed_wake = entry.wake;
            --pending_waiters_;
            if (pending_bytes_ < bytes || !active_descriptors_) std::terminate();
            pending_bytes_ -= bytes;
            --active_descriptors_;
            clear_slot_locked(entry);
            guard.unlock();
            const auto status = ores_native_actor_scheduler_fail_reserved_wake(
                failed_scheduler, failed_wake);
            if (status == ORES_NATIVE_WAKE_INVALID)
                throw std::logic_error("reserved async mailbox wake lost ownership");
            return Registration::Failed;
        }

        push_fifo_locked(handle.slot);
        return Registration::Waiting;
    }

    // Convenience for host callers which have already completed preparation.
    // Source send_async lowering should use begin_preparation() first so costly
    // payload work is bounded by the target-side descriptor quota.
    Registration register_waiter(
            std::size_t bytes,
            OresNativeActorScheduler* scheduler,
            OresNativeWakeTicket wake,
            Handle* out) {
        Handle handle{};
        if (!begin_preparation(&handle)) {
            if (out) *out = {};
            return Registration::Rejected;
        }
        if (out) *out = handle;
        const auto result = register_prepared(handle, bytes, scheduler, wake);
        if (result == Registration::Rejected) {
            if (!cancel_preparation(handle))
                throw std::logic_error("async mailbox preparation ownership lost");
        }
        return result;
    }

    // Producer cancellation wins only while the descriptor is still WAITING.
    bool cancel(Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Waiting) return false;
        if (!ores_native_actor_scheduler_cancel_wake(entry->scheduler, entry->wake))
            return false;
        if (pending_waiters_ == 0 || pending_bytes_ < entry->bytes)
            std::terminate();
        --pending_waiters_;
        pending_bytes_ -= entry->bytes;
        if (!active_descriptors_) std::terminate();
        --active_descriptors_;
        entry->state = State::Cancelled;
        return true;
    }

    /**
     * Physical ACK returns one already-charged USER credit. The credit is
     * transferred directly to the FIFO head while the accounting lock is held;
     * a racing synchronous sender never observes the intermediate free slot.
     */
    void release_acquired(std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("mailbox byte charge must be positive");
        std::unique_lock<std::mutex> guard(mutex_);
        if (!used_frames_ || used_bytes_ < bytes)
            throw std::logic_error("async mailbox credit underflow");
        --used_frames_;
        used_bytes_ -= bytes;
        for (;;) {
            DeferredFailure failure{};
            const bool progressed = service_one_waiter_locked(failure);
            if (failure.scheduler) {
                guard.unlock();
                const auto status = ores_native_actor_scheduler_fail_reserved_wake(
                    failure.scheduler, failure.wake);
                if (status == ORES_NATIVE_WAKE_INVALID)
                    throw std::logic_error("reserved async mailbox wake lost ownership");
                guard.lock();
                continue;
            }
            if (!progressed) break;
        }
    }

    /**
     * Caller has now published the immutable envelope into the transferred
     * target credit. Only then may the producer continuation be made READY.
     * Producer STOP consumes the wake loan without rolling target admission
     * back; admission is already irrevocable after transfer.
     */
    OresNativeWakePublishStatus commit(Handle handle) {
        OresNativeActorScheduler* scheduler = nullptr;
        OresNativeWakeTicket wake{};
        std::size_t retained_bytes = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Slot* entry = matching_locked(handle);
            if (!entry || entry->state != State::Transferred)
                return ORES_NATIVE_WAKE_INVALID;
            entry->state = State::Publishing;
            scheduler = entry->scheduler;
            wake = entry->wake;
            retained_bytes = entry->bytes;
        }

        // Never publish a continuation while holding mailbox accounting.
        const auto status = ores_native_actor_scheduler_publish_wake(
            scheduler, wake, ORES_NATIVE_WAKE_READY);

        std::lock_guard<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Publishing)
            throw std::logic_error("async mailbox publish ownership corrupted");
        if (status == ORES_NATIVE_WAKE_PUBLISHED
                || status == ORES_NATIVE_WAKE_PRODUCER_RETIRED) {
            if (pending_bytes_ < retained_bytes || !active_descriptors_)
                std::terminate();
            pending_bytes_ -= retained_bytes;
            --active_descriptors_;
            clear_slot_locked(*entry);
        } else {
            entry->state = State::Transferred;
        }
        return status;
    }

    // Publication failed after irrevocable transfer but before an envelope
    // became visible. This is runtime failure, not late producer cancellation.
    bool abort_transferred(Handle handle) {
        std::unique_lock<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Transferred) return false;
        if (!ores_native_actor_scheduler_abort_transferred_wake(
                entry->scheduler, entry->wake))
            return false;
        if (!used_frames_ || used_bytes_ < entry->bytes) std::terminate();
        --used_frames_;
        used_bytes_ -= entry->bytes;
        if (pending_bytes_ < entry->bytes || !active_descriptors_)
            std::terminate();
        pending_bytes_ -= entry->bytes;
        --active_descriptors_;
        clear_slot_locked(*entry);

        // The rollback created real free capacity. Preserve FIFO priority by
        // handing it directly to the next waiter before a synchronous sender
        // can observe it. Failure wake publication still occurs unlocked.
        for (;;) {
            DeferredFailure failure{};
            const bool progressed = service_one_waiter_locked(failure);
            if (failure.scheduler) {
                guard.unlock();
                const auto status = ores_native_actor_scheduler_fail_reserved_wake(
                    failure.scheduler, failure.wake);
                if (status == ORES_NATIVE_WAKE_INVALID)
                    throw std::logic_error("reserved async mailbox wake lost ownership");
                guard.lock();
                continue;
            }
            if (!progressed) break;
        }
        return true;
    }

    /**
     * Seal new admission and fail every still-WAITING producer. TRANSFERRED
     * descriptors remain irrevocable and must commit/abort explicitly.
     */
    void seal() {
        std::unique_lock<std::mutex> guard(mutex_);
        sealed_ = true;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& entry = slots_[i];
            if (entry.state == State::Preparing) {
                if (!active_descriptors_) std::terminate();
                --active_descriptors_;
                entry.state = State::Cancelled;
                continue;
            }
            if (entry.state != State::Waiting) continue;
            OresNativeActorScheduler* scheduler = entry.scheduler;
            const OresNativeWakeTicket wake = entry.wake;
            if (!pending_waiters_ || pending_bytes_ < entry.bytes
                    || !active_descriptors_)
                std::terminate();
            --pending_waiters_;
            pending_bytes_ -= entry.bytes;
            --active_descriptors_;
            entry.state = State::Cancelled;
            guard.unlock();
            const auto status = ores_native_actor_scheduler_fail_reserved_wake(
                scheduler, wake);
            if (status == ORES_NATIVE_WAKE_INVALID)
                throw std::logic_error("reserved async mailbox wake lost ownership");
            guard.lock();
        }
    }

    bool transferred(Handle handle) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const Slot* entry = matching_locked(handle);
        return entry && entry->state == State::Transferred;
    }

    std::size_t used_frames() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return used_frames_;
    }
    std::size_t used_bytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return used_bytes_;
    }
    std::size_t active_descriptors() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return active_descriptors_;
    }
    std::size_t pending_waiters() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return pending_waiters_;
    }
    std::size_t pending_bytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return pending_bytes_;
    }
    bool sealed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return sealed_;
    }

private:
    enum class State { Empty, Preparing, Waiting, Transferred, Publishing, Cancelled };
    struct Slot {
        State state = State::Empty;
        std::uint64_t generation = 0;
        std::size_t bytes = 0;
        OresNativeActorScheduler* scheduler = nullptr;
        OresNativeWakeTicket wake{};
    };

    bool fits_locked(std::size_t bytes) const noexcept {
        return used_frames_ < max_frames_
            && bytes <= max_bytes_ - used_bytes_;
    }

    std::size_t find_empty_slot_locked() {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Empty
                    || slots_[i].state == State::Cancelled)
                return i;
        return max_waiters_;
    }

    Slot* matching_locked(Handle handle) {
        if (handle.slot >= slots_.size() || !handle.generation) return nullptr;
        Slot& entry = slots_[handle.slot];
        return entry.generation == handle.generation ? &entry : nullptr;
    }
    const Slot* matching_locked(Handle handle) const {
        return const_cast<NativeAsyncMailboxAdmission*>(this)->matching_locked(handle);
    }

    void clear_slot_locked(Slot& entry) noexcept {
        entry.state = State::Empty;
        entry.bytes = 0;
        entry.scheduler = nullptr;
        entry.wake = {};
    }

    bool transfer_locked(Slot& entry) {
        return ores_native_actor_scheduler_transfer_wake(
            entry.scheduler, entry.wake) != 0;
    }

    void compact_fifo_locked() {
        if (!fifo_entries_) return;
        std::size_t write = 0;
        for (std::size_t n = 0; n < fifo_entries_; ++n) {
            const Handle queued = fifo_[(fifo_head_ + n) % max_waiters_];
            const Slot* entry = matching_locked(queued);
            if (entry && entry->state == State::Waiting)
                fifo_[write++] = queued;
        }
        fifo_head_ = 0;
        fifo_entries_ = write;
        fifo_tail_ = write % max_waiters_;
    }

    void push_fifo_locked(std::size_t slot) {
        if (fifo_entries_ == max_waiters_) compact_fifo_locked();
        if (fifo_entries_ == max_waiters_)
            throw std::logic_error("async mailbox FIFO accounting corrupted");
        fifo_[fifo_tail_] = Handle{slot, slots_[slot].generation};
        fifo_tail_ = (fifo_tail_ + 1) % max_waiters_;
        ++fifo_entries_;
    }

    struct DeferredFailure {
        OresNativeActorScheduler* scheduler = nullptr;
        OresNativeWakeTicket wake{};
    };

    bool service_one_waiter_locked(DeferredFailure& failure) {
        while (fifo_entries_) {
            const Handle queued = fifo_[fifo_head_];
            Slot* matched = matching_locked(queued);
            if (!matched || matched->state != State::Waiting) {
                fifo_head_ = (fifo_head_ + 1) % max_waiters_;
                --fifo_entries_;
                continue;
            }
            Slot& entry = *matched;
            if (!fits_locked(entry.bytes)) return false;

            fifo_head_ = (fifo_head_ + 1) % max_waiters_;
            --fifo_entries_;
            if (transfer_locked(entry)) {
                if (!pending_waiters_ || pending_bytes_ < entry.bytes)
                    std::terminate();
                --pending_waiters_;
                ++used_frames_;
                used_bytes_ += entry.bytes;
                entry.state = State::Transferred;
                return true;
            }

            // Target STOP/seal won before irreversible transfer. Remove the
            // descriptor under the accounting lock, then publish FAILED only
            // after the lock is released.
            failure.scheduler = entry.scheduler;
            failure.wake = entry.wake;
            if (!pending_waiters_ || pending_bytes_ < entry.bytes)
                std::terminate();
            --pending_waiters_;
            pending_bytes_ -= entry.bytes;
            if (!active_descriptors_) std::terminate();
            --active_descriptors_;
            clear_slot_locked(entry);
            return true;
        }
        return false;
    }

    const std::size_t max_frames_;
    const std::size_t max_bytes_;
    const std::size_t max_waiters_;
    const std::size_t max_pending_bytes_;

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::vector<Handle> fifo_;
    std::size_t fifo_head_ = 0;
    std::size_t fifo_tail_ = 0;
    std::size_t fifo_entries_ = 0;

    std::size_t used_frames_ = 0;
    std::size_t used_bytes_ = 0;
    std::size_t active_descriptors_ = 0;
    std::size_t pending_waiters_ = 0;
    std::size_t pending_bytes_ = 0;
    bool sealed_ = false;
};

} // namespace oreslang::runtime
