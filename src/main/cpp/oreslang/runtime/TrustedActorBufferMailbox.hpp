#pragma once
#include "oreslang/runtime/TrustedActorBufferPool.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// HOST-ONLY, single-recipient, bounded DATA queue for trusted actors inside
// one tenant pool. Not source actor send lowering and not an untrusted sandbox.
// Caller identities MUST be authenticated by the real actor scheduler first.
class TrustedActorBufferMailbox final {
public:
    using Handle = TrustedActorBufferPool::Handle;
    static_assert(std::is_nothrow_move_assignable<Handle>::value,
                  "mailbox publication must not throw after ownership commit");
    static_assert(std::is_nothrow_move_constructible<Handle>::value,
                  "mailbox delivery must not allocate or throw");

    TrustedActorBufferMailbox(std::shared_ptr<TrustedActorBufferPool> pool,
                              TrustedBufferActor recipient, std::size_t capacity)
        : pool_(std::move(pool)), recipient_(recipient), entries_(capacity) {
        if (!pool_ || !recipient_.id || !recipient_.incarnation || !capacity)
            throw std::invalid_argument("mailbox requires a pool, recipient and slots");
    }
    TrustedActorBufferMailbox(const TrustedActorBufferMailbox&) = delete;
    TrustedActorBufferMailbox& operator=(const TrustedActorBufferMailbox&) = delete;
    ~TrustedActorBufferMailbox() noexcept { if (!close()) std::terminate(); }

    enum class Admission { Accepted, Closed, Full };

    // Reserve mailbox credit BEFORE the irreversible ownership take.
    // Rejections leave sender ownership and its generation unchanged.
    // Forged/stale/borrowed handles still throw (host contract violation).
    Admission try_send(const Handle& owned, TrustedBufferActor sender) {
        std::lock_guard<std::mutex> guard(mutex_);
        // Invalid guest-derived handles must never masquerade as ordinary
        // backpressure. This validation is read-only; the take below is the
        // single irreversible ownership commit after slot admission.
        pool_->require_transferable(owned, sender);
        if (closed_) return Admission::Closed;
        if (count_ == entries_.size()) return Admission::Full;
        Handle transferred = pool_->take(owned, sender, recipient_);
        entries_[tail_] = std::move(transferred); // noexcept
        tail_ = (tail_ + 1) % entries_.size();
        ++count_;
        return Admission::Accepted;
    }
    void send(const Handle& owned, TrustedBufferActor sender) {
        const Admission admission = try_send(owned, sender);
        if (admission == Admission::Closed) throw std::logic_error("buffer mailbox closed");
        if (admission == Admission::Full) throw std::logic_error("buffer mailbox full");
    }

    Handle receive(TrustedBufferActor reader) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!(reader == recipient_)) throw std::logic_error("incorrect mailbox recipient");
        if (closed_) throw std::logic_error("buffer mailbox closed");
        if (!count_) throw std::logic_error("buffer mailbox empty");
        Handle out = std::move(entries_[head_]); // noexcept
        head_ = (head_ + 1) % entries_.size();
        --count_;
        return out;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return count_;
    }

    // Terminal close rejects new messages, then drops previously admitted
    // but unread DATA as the receiver. The pool is retained by shared_ptr,
    // so a mailbox cannot dereference a destructed pool. If the pool was
    // already retired, its storage has already been revoked/zeroed.
    // Do not throw away the only queued owner handle when immutable loans
    // still pin the tenant pool. The caller may retry after loan release.
    // Caller must serialize guest loan admission with this close operation
    // using the trusted scheduler gate; direct pool use is host-only.
    bool close() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return true;
        if (!pool_->is_retired()) {
            // A successfully published, unread DATA slot must remain owned
            // by this queue. Never strand its handle on a failed drop.
            for (std::size_t i = 0; i < count_; ++i) {
                const auto& h = entries_[(head_ + i) % entries_.size()];
                try {
                    if (pool_->has_active_loans_for_handle(h, recipient_))
                        return false;
                } catch (...) {
                    if (!pool_->is_retired()) std::terminate();
                    break;
                }
            }
        }
        closed_ = true;
        while (count_) {
            Handle abandoned = std::move(entries_[head_]);
            head_ = (head_ + 1) % entries_.size();
            --count_;
            try { pool_->drop(abandoned, recipient_); }
            catch (...) {
                // A previously retired pool has already invalidated every
                // handle. Any other unexpected loss of ownership is a
                // fatal host invariant violation, never silently swallowed.
                if (!pool_->is_retired()) std::terminate();
            }
        }
        return true;
    }

private:
    std::shared_ptr<TrustedActorBufferPool> pool_;
    const TrustedBufferActor recipient_;
    mutable std::mutex mutex_;
    std::vector<Handle> entries_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    bool closed_ = false;
};

} // namespace oreslang::runtime
