#pragma once

#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/ActorShardColumns.hpp"
#include "oreslang/runtime/NativeTransportCallbackGate.hpp"
#include "oreslang/runtime/ProxyRequestSlab.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

// Unique within this native process (not a cryptographic capability).
// Saturate permanently on exhaustion; fetch_add wrapping would eventually
// reissue a slab identity and allow a stale completion envelope to match.
inline std::atomic<std::uint64_t> next_proxy_slab_identity{1};
inline std::uint64_t reserve_proxy_slab_identity() {
    auto next = next_proxy_slab_identity.load(std::memory_order_relaxed);
    for (;;) {
        if (next == 0 || next == UINT64_MAX) {
            throw std::overflow_error("proxy slab identity exhausted");
        }
        if (next_proxy_slab_identity.compare_exchange_weak(
                next, next + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) return next;
    }
}

/**
 * Exact-generation native actor wrapper over *physical ActorArena persistent
 * storage*. The runtime's checked arena API owns the request words; no host
 * vector mirrors the slab. Owner and VM shard ledgers are reserved atomically
 * before the arena is created; padded physical page capacity is charged.
 *
 * Each public operation enters/leaves one native arena turn, following logical
 * actor ownership across carrier migration. This is intentionally slower than
 * a future actor-turn-batched kernel, but keeps the existing arena guard and
 * its 1 MiB-per-persistent-slot policy rather than exporting raw pointers.
 *
 * Trusted host must register one move-only callback permit before enqueueing
 * each I/O completion/timer/waker that may touch this arena. Shutdown freezes
 * admissions and drains those permits, then the transport gate issues a
 * generation/slab-bound DrainReceipt. Only that receipt can authorize close
 * on the owning actor turn; logical live()==0 alone is never sufficient.
 * This proof covers registered host callbacks, not unregistered OS activity.
 * Any live request remains fail-stop. This object and its budgets must outlive
 * all associated callback envelopes; teardown must precede destruction of
 * scheduler and budget ledgers. No Oreslang source lowering is provided.
 */
class ActorOwnedProxyRequestSlab final {
public:
    using Layout = ProxyRequestSlab::Layout;

    struct Receipt final {
        std::uint64_t actor_domain = 0;
        std::uint64_t actor_slot = 0;
        std::uint64_t actor_generation = 0;
        std::uint64_t slab_identity = 0;
        std::uint64_t request = 0;
        std::uint64_t claim = 0;
    };

    ActorOwnedProxyRequestSlab(OresNativeActorScheduler* scheduler,
            OresNativeActorRef owner,
            ActorShardBudget& actor_budget, ActorShardBudget& vm_budget,
            Layout layout, std::uint32_t capacity, std::uint32_t lanes)
        : scheduler_(scheduler), owner_(owner), actor_budget_(actor_budget),
          vm_budget_(vm_budget), identity_(reserve_proxy_slab_identity()),
          callback_gate_(owner_.scheduler_domain, owner_.actor_slot,
                         owner_.generation, identity_) {
        require_owner();
        if (!identity_ || identity_ == UINT64_MAX || owner_.actor_slot == UINT64_MAX
                || &actor_budget_ == &vm_budget_
                || !actor_budget_.matches_owner(scheduler_, owner_)
                || !vm_budget_.vm_wide()) {
            throw std::invalid_argument("proxy slab owner, identity or budget mismatch");
        }
        const auto words = ProxyRequestSlab::required_words(layout, capacity, lanes);
        const auto bytes = words * sizeof(std::uint32_t);
        // Existing ActorArena.h deliberately limits one persistent slot to
        // 1 MiB. Pages are actor-owned; no raw pointer is exported.
        page_bytes_ = std::min<std::size_t>(bytes, 1024U * 1024U);
        pages_count_ = (bytes + page_bytes_ - 1) / page_bytes_;
        if (pages_count_ == 0 || pages_count_ > handles_.size()) {
            throw std::length_error("native proxy page count exceeded");
        }
        reserved_bytes_ = pages_count_ * page_bytes_;
        if (!ActorShardBudget::try_reserve_pair(
                actor_budget_, vm_budget_, reserved_bytes_)) {
            throw std::length_error("proxy slab actor/VM quota exceeded");
        }
        try {
            arena_ = ores_actor_arena_open(owner_.actor_slot + 1,
                owner_.generation, pages_count_, page_bytes_, 1);
            ArenaTurn session(arena_, owner_);
            for (std::size_t page = 0; page < pages_count_; ++page) {
                handles_[page] = ores_actor_arena_alloc(
                    arena_, session.turn(), page_bytes_);
            }
            slab_ = std::make_unique<ProxyRequestSlab>(layout, capacity, lanes,
                arena_, session.turn(),
                std::span<const OresArenaHandle>(handles_.data(), pages_count_),
                page_bytes_ / sizeof(std::uint32_t));
        } catch (...) {
            if (arena_) {
                ores_actor_arena_retire(arena_);
                if (!ores_actor_arena_try_destroy(arena_)) std::abort();
                arena_ = nullptr;
            }
            ActorShardBudget::release_pair(
                actor_budget_, vm_budget_, reserved_bytes_);
            throw;
        }
    }

    ActorOwnedProxyRequestSlab(const ActorOwnedProxyRequestSlab&) = delete;
    ActorOwnedProxyRequestSlab& operator=(const ActorOwnedProxyRequestSlab&) = delete;
    ActorOwnedProxyRequestSlab(ActorOwnedProxyRequestSlab&&) = delete;
    ActorOwnedProxyRequestSlab& operator=(ActorOwnedProxyRequestSlab&&) = delete;

    ~ActorOwnedProxyRequestSlab() {
        if (!arena_ || !slab_) std::abort();
        if (!closed_) {
            // An actor may have drained all requests but missed explicit
            // close() during terminal cancellation. Permit trusted host
            // cleanup ONLY after the scheduler-done retirement fence.
            // A retired logical actor is not an external callback join.
            // An explicit owner-turn host quiescence assertion is required.
            if (!actor_budget_.retired() || !transport_quiescent_
                    || slab_->live() != 0) std::abort();
            ArenaTurn session(arena_, owner_);
            slab_->resume(session.turn());
            slab_->close();
            closed_ = true;
        }
        slab_.reset();
        ores_actor_arena_retire(arena_);
        if (!ores_actor_arena_try_destroy(arena_)) std::abort();
        arena_ = nullptr;
        ActorShardBudget::release_pair(
            actor_budget_, vm_budget_, reserved_bytes_);
    }

    // Identity is correlation metadata, not ambient cross-actor authority.
    std::uint64_t identity() const {
        require_owner();
        if (closed_) throw std::logic_error("proxy request slab is closed");
        return identity_;
    }
    std::uint32_t capacity() const {
        return in_turn([&] { return slab_->capacity(); });
    }
    std::uint32_t live() const {
        return in_turn([&] { return slab_->live(); });
    }
    bool full() const {
        return in_turn([&] { return slab_->full(); });
    }
    std::uint64_t admit(std::uint32_t epoch, std::uint32_t connection,
                        std::uint32_t route) {
        return in_turn([&] {
            // This permit is the admission linearization point. A concurrent
            // freeze either wins first (reject) or must count this operation
            // until it returns, including the underlying native slab write.
            auto admission = callback_gate_.try_admit();
            if (!admission || transport_quiescent_) {
                throw std::logic_error("proxy slab transport is closing");
            }
            return slab_->admit(epoch, connection, route);
        });
    }
    std::uint32_t route(std::uint64_t ticket) const {
        return in_turn([&] { return slab_->route(ticket); });
    }
    std::uint32_t connection(std::uint64_t ticket) const {
        return in_turn([&] { return slab_->connection(ticket); });
    }
    std::uint32_t epoch(std::uint64_t ticket) const {
        return in_turn([&] { return slab_->epoch(ticket); });
    }
    std::uint32_t pending(std::uint64_t ticket) const {
        return in_turn([&] { return slab_->pending(ticket); });
    }

    // Trusted host completion dispatcher must retain the entire envelope.
    // The claim alone is NOT enough to prove the destination slab/actor.
    Receipt pin(std::uint64_t ticket) {
        return in_turn([&] {
            // Never check a separate accepting() flag then perform an
            // uncounted callback pin: shutdown can race in between.
            auto admission = callback_gate_.try_admit();
            if (!admission || transport_quiescent_) {
                throw std::logic_error("proxy slab transport is closing");
            }
            auto claim = slab_->pin(ticket);
            return claim ? Receipt{owner_.scheduler_domain, owner_.actor_slot,
                owner_.generation, identity_, ticket, claim} : Receipt{};
        });
    }
    bool settle(Receipt receipt) {
        return in_turn([&] {
            if (receipt.actor_domain != owner_.scheduler_domain
                    || receipt.actor_slot != owner_.actor_slot
                    || receipt.actor_generation != owner_.generation
                    || receipt.slab_identity != identity_
                    || receipt.claim == 0 || receipt.request == 0) return false;
            const auto index = static_cast<std::uint32_t>(receipt.claim);
            if (index == 0 || (index - 1) / ProxyRequestSlab::kPins
                    != static_cast<std::uint32_t>(receipt.request)) return false;
            try {
                (void) slab_->pending(receipt.request); // reject recycled incarnation
            } catch (const std::logic_error&) {
                return false;
            }
            return slab_->settle(receipt.claim);
        });
    }
    bool retire(std::uint64_t ticket) {
        return in_turn([&] { return slab_->retire(ticket); });
    }

    // Export only to trusted transport/supervisor code on the owner actor
    // lane. It must retain a Permit from enqueue until callback teardown
    // completes, even if the callback runs on a different physical carrier.
    NativeTransportCallbackGate& transport_callbacks() {
        require_owner();
        if (closed_) throw std::logic_error("proxy request slab is closed");
        return callback_gate_;
    }

    // Nonblocking: transport first freezes admission, joins/revokes all
    // registered asynchronous callbacks, and obtains a DrainReceipt. The
    // receipt must originate from THIS slab's exact actor generation.
    // Admission, freeze and drain use one bounded atomic stop/count word.
    void confirm_transport_quiescence(
            const NativeTransportCallbackGate::DrainReceipt& receipt) {
        in_turn([&] {
            if (!callback_gate_.verifies(receipt)) {
                throw std::logic_error("invalid or undrained transport receipt");
            }
            if (slab_->live() != 0) {
                throw std::logic_error(
                    "cannot confirm transport quiescence with live requests");
            }
            transport_quiescent_ = true;
        });
    }

    void close() {
        require_owner();
        if (closed_) return;
        if (!transport_quiescent_) {
            throw std::logic_error("transport quiescence not confirmed");
        }
        in_turn([&] { slab_->close(); });
        closed_ = true;
    }

private:
    struct ArenaTurn final {
        OresActorArena* arena;
        OresArenaTurn value;
        ArenaTurn(OresActorArena* a, OresNativeActorRef actor)
            : arena(a),
              value(ores_actor_arena_enter(
                  a, actor.actor_slot + 1, actor.generation)) {}
        ~ArenaTurn() { ores_actor_arena_leave(arena, value); }
        ArenaTurn(const ArenaTurn&) = delete;
        ArenaTurn& operator=(const ArenaTurn&) = delete;
        OresArenaTurn turn() const { return value; }
    };

    void require_owner() const {
        if (!scheduler_ || actor_budget_.retired()
                || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("proxy slab requires exact live actor turn");
        }
    }
    template<class F>
    auto in_turn(F&& function) const -> decltype(function()) {
        require_owner();
        if (closed_) throw std::logic_error("proxy request slab is closed");
        ArenaTurn session(arena_, owner_);
        slab_->resume(session.turn());
        return std::forward<F>(function)();
    }

    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef owner_;
    ActorShardBudget& actor_budget_;
    ActorShardBudget& vm_budget_;
    std::uint64_t identity_;
    NativeTransportCallbackGate callback_gate_;
    std::size_t reserved_bytes_ = 0;
    std::size_t page_bytes_ = 0;
    std::size_t pages_count_ = 0;
    std::array<OresArenaHandle, 16> handles_{};
    OresActorArena* arena_ = nullptr;
    std::unique_ptr<ProxyRequestSlab> slab_;
    // Monotonic recorded verification of a gate-issued DrainReceipt.
    // Never inferred solely from slab_->live()==0 or scheduler retirement.
    bool transport_quiescent_ = false;
    bool closed_ = false;
};

} // namespace oreslang::runtime
