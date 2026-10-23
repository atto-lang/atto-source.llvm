#pragma once

#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/ActorShardColumns.hpp"
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
inline std::atomic<std::uint64_t> next_proxy_slab_identity{1};

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
 * Host must fence external I/O and run close() on the owning actor turn, then
 * retire actor budget after scheduler done. This object and its budgets must
 * outlive all associated callback envelopes; teardown must precede destruction
 * of scheduler and budget ledgers. No Oreslang source lowering is provided.
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
          vm_budget_(vm_budget), identity_(next_proxy_slab_identity.fetch_add(
                  1, std::memory_order_relaxed)) {
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
        // Never silently free a backing allocation when completion handlers
        // might still hold it. Explicit owner-turn close is a release gate.
        if (!closed_ || !arena_) std::abort();
        slab_.reset();
        ores_actor_arena_retire(arena_);
        if (!ores_actor_arena_try_destroy(arena_)) std::abort();
        arena_ = nullptr;
        ActorShardBudget::release_pair(
            actor_budget_, vm_budget_, reserved_bytes_);
    }

    std::uint64_t identity() const noexcept { return identity_; }
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
        return in_turn([&] { return slab_->admit(epoch, connection, route); });
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
    void close() {
        require_owner();
        if (closed_) return;
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
        ArenaTurn session(arena_, owner_);
        slab_->resume(session.turn());
        return std::forward<F>(function)();
    }

    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef owner_;
    ActorShardBudget& actor_budget_;
    ActorShardBudget& vm_budget_;
    std::uint64_t identity_;
    std::size_t reserved_bytes_ = 0;
    std::size_t page_bytes_ = 0;
    std::size_t pages_count_ = 0;
    std::array<OresArenaHandle, 16> handles_{};
    OresActorArena* arena_ = nullptr;
    std::unique_ptr<ProxyRequestSlab> slab_;
    bool closed_ = false;
};

} // namespace oreslang::runtime
