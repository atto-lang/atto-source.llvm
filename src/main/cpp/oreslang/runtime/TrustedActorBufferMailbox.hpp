#pragma once
#include "oreslang/runtime/TrustedActorBufferPool.hpp"

#include <cstddef>
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
    ~TrustedActorBufferMailbox() { close(); }

    // Reserve a queue slot under the publication mutex, then move ownership.
    // There is no allocation or throwable publication action after take().
    // If quota, ownership, loan or close validation fails, the old handle
    // remains owned by the sender and nothing is enqueued.
    void send(const Handle& owned, TrustedBufferActor sender) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("buffer mailbox closed");
        if (count_ == entries_.size()) throw std::logic_error("buffer mailbox full");
        Handle transferred = pool_->take(owned, sender, recipient_);
        entries_[tail_] = std::move(transferred); // noexcept
        tail_ = (tail_ + 1) % entries_.size();
        ++count_;
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
    void close() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return;
        closed_ = true;
        while (count_) {
            Handle abandoned = std::move(entries_[head_]);
            head_ = (head_ + 1) % entries_.size();
            --count_;
            try { pool_->drop(abandoned, recipient_); }
            catch (...) { /* pool retirement already revoked ownership */ }
        }
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
