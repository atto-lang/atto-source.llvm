#pragma once

#include "oreslang/runtime/TrustedActorBufferMailbox.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace oreslang::runtime {

// Trusted HOST adapter, not a guest ABI or an OS security boundary.
// Only the actor scheduler may call register_actor/enter_turn/retire_actor.
// Guest code cannot construct a Turn or select arbitrary actor identities.
class TrustedActorBufferSchedulerGate final {
    using Handle = TrustedActorBufferPool::Handle;
    using Mailbox = TrustedActorBufferMailbox;

    struct Record final {
        std::uint64_t incarnation = 0;
        bool live = false;
        bool in_turn = false;
        std::shared_ptr<Mailbox> mailbox;
    };
    struct State final {
        explicit State(std::shared_ptr<TrustedActorBufferPool> p)
            : pool(std::move(p)) {}
        mutable std::mutex mutex;
        std::shared_ptr<TrustedActorBufferPool> pool;
        std::unordered_map<std::uint64_t, Record> actors;
        bool closed = false;
    };

public:
    class Turn final {
        friend class TrustedActorBufferSchedulerGate;
        std::shared_ptr<State> state_;
        TrustedBufferActor actor_{};
        bool active_ = false;
        Turn(std::shared_ptr<State> s, TrustedBufferActor a)
            : state_(std::move(s)), actor_(a), active_(true) {}
    public:
        Turn() = delete;
        Turn(const Turn&) = delete;
        Turn& operator=(const Turn&) = delete;
        Turn(Turn&& other) noexcept
            : state_(std::move(other.state_)), actor_(other.actor_),
              active_(std::exchange(other.active_, false)) {}
        Turn& operator=(Turn&&) = delete;
        ~Turn() { release(); }
        void release() noexcept {
            if (!active_) return;
            std::lock_guard<std::mutex> lock(state_->mutex);
            const auto it = state_->actors.find(actor_.id);
            if (it != state_->actors.end() &&
                it->second.incarnation == actor_.incarnation)
                it->second.in_turn = false;
            active_ = false;
        }
    };

    explicit TrustedActorBufferSchedulerGate(
            std::shared_ptr<TrustedActorBufferPool> pool)
        : state_(std::make_shared<State>(std::move(pool))) {
        if (!state_->pool) throw std::invalid_argument("missing trusted buffer pool");
        state_->pool->claim_scheduler_owner();
    }
    TrustedActorBufferSchedulerGate(const TrustedActorBufferSchedulerGate&) = delete;
    TrustedActorBufferSchedulerGate& operator=(const TrustedActorBufferSchedulerGate&) = delete;
    ~TrustedActorBufferSchedulerGate() noexcept {
        // Destroying the native gate with a live turn would orphan authority.
        if (!close()) std::terminate();
    }

    // Host must establish authenticated actor identity and tenant before
    // registration. Generation is strictly increasing even after retirement.
    void register_actor(TrustedBufferActor actor, std::size_t mailbox_capacity) {
        if (!actor.id || !actor.incarnation || !mailbox_capacity)
            throw std::invalid_argument("invalid admitted actor");
        std::lock_guard<std::mutex> lock(state_->mutex);
        require_open();
        auto it = state_->actors.find(actor.id);
        if (it != state_->actors.end() &&
            (it->second.live || it->second.incarnation >= actor.incarnation))
            throw std::logic_error("live or replayed actor incarnation");
        auto mailbox = std::make_shared<Mailbox>(state_->pool, actor, mailbox_capacity);
        if (it == state_->actors.end())
            state_->actors.emplace(actor.id, Record{actor.incarnation, true, false,
                                                     std::move(mailbox)});
        else
            it->second = Record{actor.incarnation, true, false, std::move(mailbox)};
    }

    Turn enter_turn(TrustedBufferActor actor) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        require_open();
        auto& rec = require_actor(actor);
        if (rec.in_turn) throw std::logic_error("actor already has an active turn");
        rec.in_turn = true;
        return Turn(state_, actor);
    }

    Handle allocate(const Turn& turn, std::size_t bytes) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto actor = require_turn(turn);
        return state_->pool->allocate(actor, bytes);
    }
    void write(const Turn& turn, const Handle& buffer,
               std::size_t offset, std::uint8_t value) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pool->write(buffer, require_turn(turn), offset, value);
    }
    std::uint8_t read(const Turn& turn, const Handle& buffer,
                      std::size_t offset) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->pool->read(buffer, require_turn(turn), offset);
    }
    void drop(const Turn& turn, const Handle& buffer) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pool->drop(buffer, require_turn(turn));
    }

    // Recipient identity and liveness are checked under the same gate as
    // send publication. The mailbox then reserves a slot BEFORE pool.take().
    void send(const Turn& sender, TrustedBufferActor recipient,
              const Handle& buffer) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto from = require_turn(sender);
        auto& target = require_actor(recipient);
        target.mailbox->send(buffer, from);
    }
    Handle receive(const Turn& recipient) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto actor = require_turn(recipient);
        return require_actor(actor).mailbox->receive(actor);
    }
    std::size_t pending(TrustedBufferActor actor) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return require_actor(actor).mailbox->pending();
    }

    // Only allowed after the owning turn exits. Closing a receiver mailbox
    // first frees queued messages; remaining receiver-owned buffers are
    // reclaimed only when no loan remains. The pool preflight runs BEFORE
    // any actor lifecycle state or queue is changed.
    void retire_actor(TrustedBufferActor actor) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& rec = require_actor(actor);
        if (rec.in_turn) throw std::logic_error("active turn blocks retirement");
        state_->pool->require_actor_quiescent(actor);
        rec.mailbox->close();
        state_->pool->reclaim_actor(actor);
        rec.live = false;
        rec.mailbox.reset();
    }

    bool close() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) return true;
        for (const auto& item : state_->actors)
            if (item.second.in_turn) return false;
        state_->closed = true;
        for (auto& item : state_->actors) {
            if (item.second.mailbox) item.second.mailbox->close();
            item.second.live = false;
        }
        state_->pool->retire();
        return true;
    }

private:
    void require_open() const {
        if (state_->closed) throw std::logic_error("actor scheduler gate closed");
    }
    Record& require_actor(TrustedBufferActor actor) const {
        require_open();
        auto it = state_->actors.find(actor.id);
        if (it == state_->actors.end() || !it->second.live ||
            it->second.incarnation != actor.incarnation)
            throw std::logic_error("unknown or stale actor incarnation");
        return it->second;
    }
    TrustedBufferActor require_turn(const Turn& turn) const {
        require_open();
        if (turn.state_ != state_ || !turn.active_)
            throw std::logic_error("foreign or released execution turn");
        auto& actor = require_actor(turn.actor_);
        if (!actor.in_turn) throw std::logic_error("execution turn not admitted");
        return turn.actor_;
    }
    std::shared_ptr<State> state_;
};
static_assert(!std::is_default_constructible<TrustedActorBufferSchedulerGate::Turn>::value,
              "guest callers must not fabricate a scheduler turn");
static_assert(!std::is_copy_constructible<TrustedActorBufferSchedulerGate::Turn>::value,
              "execution turn authority cannot be copied");

} // namespace oreslang::runtime
