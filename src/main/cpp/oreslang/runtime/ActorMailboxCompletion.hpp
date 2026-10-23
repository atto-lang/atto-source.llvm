#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * One bounded admission/completion domain per actor mailbox, shared across
 * URGENT/NORMAL/BULK. Sequence numbers are assigned at successful
 * publication (never at attempted admission); a failed move/allocation does
 * not consume a sequence or make a drain barrier appear complete.
 *
 * Admission also reserves a window slot before moving a sender's payload.
 * Without the window one never-completing earlier message would permit an
 * unbounded out-of-order completion set during ongoing urgent traffic.
 *
 * publish() invokes ONLY an internal prepared-envelope queue insertion; this
 * callback must not invoke guest code or re-enter the ledger. A failed insert
 * leaves the preparation reserved for cancel_prepare().
 */
class ActorMailboxCompletion final {
public:
    enum class DrainStatus { Pending, Delivered, Aborted };
    explicit ActorMailboxCompletion(std::size_t window) : done_(window, 0) {
        if (window == 0)
            throw std::invalid_argument("actor mailbox completion window must be positive");
    }

    ActorMailboxCompletion(const ActorMailboxCompletion&) = delete;
    ActorMailboxCompletion& operator=(const ActorMailboxCompletion&) = delete;

    bool reserve_prepare() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (next_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor mailbox sequence exhausted");
        const auto outstanding = next_ - 1 - watermark_;
        if (outstanding >= done_.size() ||
            preparing_ >= done_.size() - static_cast<std::size_t>(outstanding))
            return false;
        ++preparing_;
        return true;
    }

    void cancel_prepare() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!preparing_) std::terminate();
        --preparing_;
    }

    template<class Publish>
    std::uint64_t publish(Publish&& write_envelope) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!preparing_)
            throw std::logic_error("actor mailbox publication without preparation");
        if (next_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor mailbox sequence exhausted");
        const auto sequence = next_;
        // Failed queue allocation does not consume the sequence or prep slot.
        std::forward<Publish>(write_envelope)(sequence);
        ++next_;
        --preparing_;
        return sequence;
    }

    // Aborted means terminal actor teardown after physical quiescence,
    // not a successful handler invocation. It MUST fail matching drains.
    void complete(std::uint64_t sequence, bool terminal_aborted = false) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!sequence || sequence <= watermark_ || sequence >= next_
            || sequence - watermark_ > done_.size())
            throw std::logic_error("invalid or duplicate actor mailbox completion");
        const auto slot = static_cast<std::size_t>(sequence % done_.size());
        if (done_[slot])
            throw std::logic_error("duplicate actor mailbox completion");
        done_[slot] = terminal_aborted ? 2 : 1;
        if (terminal_aborted && (!first_aborted_ || sequence < first_aborted_))
            first_aborted_ = sequence;
        while (watermark_ + 1 < next_) {
            const auto next_slot =
                static_cast<std::size_t>((watermark_ + 1) % done_.size());
            if (!done_[next_slot]) break;
            done_[next_slot] = 0;
            ++watermark_;
        }
    }

    std::uint64_t snapshot() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return next_ - 1;
    }

    std::uint64_t watermark() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return watermark_;
    }

    DrainStatus drain_status(std::uint64_t sequence) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (sequence >= next_)
            throw std::invalid_argument("actor mailbox drain target was not committed");
        if (watermark_ < sequence) return DrainStatus::Pending;
        return first_aborted_ && first_aborted_ <= sequence
                ? DrainStatus::Aborted : DrainStatus::Delivered;
    }

    // Compatibility: only success can satisfy an existing positive drain
    // predicate. Use drain_status() to distinguish Pending from Aborted.
    bool completed_through(std::uint64_t sequence) const {
        return drain_status(sequence) == DrainStatus::Delivered;
    }

private:
    mutable std::mutex mutex_;
    std::vector<unsigned char> done_;
    std::uint64_t next_ = 1;
    std::uint64_t watermark_ = 0;
    std::uint64_t first_aborted_ = 0; // terminal actor generation is poisoned
    std::size_t preparing_ = 0;
};

} // namespace oreslang::runtime
