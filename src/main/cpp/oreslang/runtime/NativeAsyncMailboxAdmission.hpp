#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <cstddef>
#include <cstdint>
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
    Registration register_waiter(
            std::size_t bytes,
            OresNativeActorScheduler* scheduler,
            OresNativeWakeTicket wake,
            Handle* out) {
        if (out) *out = {};
        if (!bytes || !scheduler || !wake.slot_generation
                || !wake.descriptor_generation)
            return Registration::Rejected;

        std::lock_guard<std::mutex> guard(mutex_);
        if (sealed_ || bytes > max_bytes_
                || pending_waiters_ >= max_waiters_
                || bytes > max_pending_bytes_ - pending_bytes_)
            return Registration::Rejected;

        compact_fifo_locked();
        const std::size_t slot = find_empty_slot_locked();
        if (slot == max_waiters_) return Registration::Rejected;
        Slot& entry = slots_[slot];
        if (entry.generation == std::numeric_limits<std::uint64_t>::max())
            return Registration::Rejected;
        ++entry.generation;
        if (!entry.generation) std::terminate();
        entry.state = State::Waiting;
        entry.bytes = bytes;
        entry.scheduler = scheduler;
        entry.wake = wake;
        pending_bytes_ += bytes;
        ++pending_waiters_;

        Handle handle{slot, entry.generation};
        if (out) *out = handle;

        // No earlier waiter may be bypassed. If this is the only waiter and a
        // full credit is available now, transfer immediately under the same
        // accounting mutex that synchronous admission uses.
        if (pending_waiters_ == 1 && fifo_entries_ == 0 && fits_locked(bytes)) {
            if (transfer_locked(entry)) {
                --pending_waiters_;
                pending_bytes_ -= bytes;
                ++used_frames_;
                used_bytes_ += bytes;
                entry.state = State::Transferred;
                return Registration::Transferred;
            }
            terminalize_failed_locked(entry);
            --pending_waiters_;
            pending_bytes_ -= bytes;
            clear_slot_locked(entry);
            return Registration::Failed;
        }

        push_fifo_locked(slot);
        return Registration::Waiting;
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
        std::lock_guard<std::mutex> guard(mutex_);
        if (!used_frames_ || used_bytes_ < bytes)
            throw std::logic_error("async mailbox credit underflow");
        --used_frames_;
        used_bytes_ -= bytes;
        service_waiters_locked();
    }

    /**
     * Caller has now published the immutable envelope into the transferred
     * target credit. Only then may the producer continuation be made READY.
     * Producer STOP consumes the wake loan without rolling target admission
     * back; admission is already irrevocable after transfer.
     */
    OresNativeWakePublishStatus commit(Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Transferred)
            return ORES_NATIVE_WAKE_INVALID;
        const auto status = ores_native_actor_scheduler_publish_wake(
            entry->scheduler, entry->wake, ORES_NATIVE_WAKE_READY);
        if (status == ORES_NATIVE_WAKE_PUBLISHED
                || status == ORES_NATIVE_WAKE_PRODUCER_RETIRED)
            clear_slot_locked(*entry);
        return status;
    }

    // Publication failed after irrevocable transfer but before an envelope
    // became visible. This is runtime failure, not late producer cancellation.
    bool abort_transferred(Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* entry = matching_locked(handle);
        if (!entry || entry->state != State::Transferred) return false;
        if (!ores_native_actor_scheduler_abort_transferred_wake(
                entry->scheduler, entry->wake))
            return false;
        if (!used_frames_ || used_bytes_ < entry->bytes) std::terminate();
        --used_frames_;
        used_bytes_ -= entry->bytes;
        clear_slot_locked(*entry);
        service_waiters_locked();
        return true;
    }

    /**
     * Seal new admission and fail every still-WAITING producer. TRANSFERRED
     * descriptors remain irrevocable and must commit/abort explicitly.
     */
    void seal() {
        std::lock_guard<std::mutex> guard(mutex_);
        sealed_ = true;
        for (Slot& entry : slots_) {
            if (entry.state != State::Waiting) continue;
            terminalize_failed_locked(entry);
            if (!pending_waiters_ || pending_bytes_ < entry.bytes)
                std::terminate();
            --pending_waiters_;
            pending_bytes_ -= entry.bytes;
            entry.state = State::Cancelled;
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
    enum class State { Empty, Waiting, Transferred, Cancelled };
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

    void terminalize_failed_locked(Slot& entry) {
        const auto status = ores_native_actor_scheduler_fail_reserved_wake(
            entry.scheduler, entry.wake);
        if (status == ORES_NATIVE_WAKE_INVALID)
            throw std::logic_error("reserved async mailbox wake lost ownership");
    }

    void compact_fifo_locked() {
        if (fifo_entries_ < max_waiters_) return;
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

    void service_waiters_locked() {
        while (fifo_entries_) {
            const Handle queued = fifo_[fifo_head_];
            Slot* matched = matching_locked(queued);
            if (!matched || matched->state != State::Waiting) {
                fifo_head_ = (fifo_head_ + 1) % max_waiters_;
                --fifo_entries_;
                continue;
            }
            Slot& entry = *matched;
            if (entry.state != State::Waiting) {
                fifo_head_ = (fifo_head_ + 1) % max_waiters_;
                --fifo_entries_;
                continue;
            }
            if (!fits_locked(entry.bytes)) return;

            fifo_head_ = (fifo_head_ + 1) % max_waiters_;
            --fifo_entries_;
            if (transfer_locked(entry)) {
                if (!pending_waiters_ || pending_bytes_ < entry.bytes)
                    std::terminate();
                --pending_waiters_;
                pending_bytes_ -= entry.bytes;
                ++used_frames_;
                used_bytes_ += entry.bytes;
                entry.state = State::Transferred;
                continue;
            }

            // Target STOP/seal won before the irreversible transfer. Resume a
            // live producer with FAILED instead of silently dropping its Future.
            terminalize_failed_locked(entry);
            if (!pending_waiters_ || pending_bytes_ < entry.bytes)
                std::terminate();
            --pending_waiters_;
            pending_bytes_ -= entry.bytes;
            clear_slot_locked(entry);
        }
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
    std::size_t pending_waiters_ = 0;
    std::size_t pending_bytes_ = 0;
    bool sealed_ = false;
};

} // namespace oreslang::runtime
