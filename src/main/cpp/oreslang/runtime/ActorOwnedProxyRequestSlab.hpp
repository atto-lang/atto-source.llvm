#pragma once

#include "oreslang/runtime/ActorShardColumns.hpp"
#include "oreslang/runtime/ProxyRequestSlab.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

/**
 * Trusted native host adapter, NOT Oreslang source lowering. One exact logical
 * actor generation owns the slab across pthread carrier migrations. A request
 * operation must execute in that actor's turn. Both actor and VM budgets are
 * charged before allocating backing; all hot operations allocate no objects.
 *
 * Host must stop/drain external I/O, retire the actor budget after get_done,
 * destroy this object, and only then destroy the scheduler and budget ledgers.
 * A destructor with live request pins fails closed instead of freeing storage
 * while callbacks might still reference it.
 */
class ActorOwnedProxyRequestSlab final {
public:
    using Layout = ProxyRequestSlab::Layout;

    ActorOwnedProxyRequestSlab(OresNativeActorScheduler* scheduler,
            OresNativeActorRef owner,
            ActorShardBudget& actor_budget, ActorShardBudget& vm_budget,
            Layout layout, std::uint32_t capacity, std::uint32_t lanes)
        : scheduler_(scheduler), owner_(owner), actor_budget_(actor_budget),
          vm_budget_(vm_budget) {
        require_owner();
        if (&actor_budget_ == &vm_budget_
                || !actor_budget_.matches_owner(scheduler_, owner_)
                || !vm_budget_.vm_wide()) {
            throw std::invalid_argument("proxy slab budget owner or VM scope mismatch");
        }
        const auto words = ProxyRequestSlab::required_words(layout, capacity, lanes);
        reserved_bytes_ = words * sizeof(std::uint32_t);
        if (!ActorShardBudget::try_reserve_pair(
                actor_budget_, vm_budget_, reserved_bytes_)) {
            throw std::length_error("proxy slab actor/VM budget exceeded");
        }
        try {
            backing_.resize(words);
            slab_ = std::make_unique<ProxyRequestSlab>(layout, capacity, lanes,
                    std::span<std::uint32_t>(backing_));
        } catch (...) {
            scrub_and_free();
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
        if (slab_ && !closed_ && slab_->live() != 0) {
            // An external I/O owner failed to join cancellation/retirement.
            // Releasing the backing here could cause a use-after-free.
            std::abort();
        }
        slab_.reset();
        scrub_and_free();
        ActorShardBudget::release_pair(actor_budget_, vm_budget_, reserved_bytes_);
    }

    std::uint32_t capacity() const { require_owner(); return slab_->capacity(); }
    std::uint32_t live() const { require_owner(); return slab_->live(); }
    bool full() const { require_owner(); return slab_->full(); }
    std::uint64_t admit(std::uint32_t epoch, std::uint32_t connection,
                        std::uint32_t route) {
        require_owner(); return slab_->admit(epoch, connection, route);
    }
    std::uint32_t route(std::uint64_t ticket) const {
        require_owner(); return slab_->route(ticket);
    }
    std::uint32_t connection(std::uint64_t ticket) const {
        require_owner(); return slab_->connection(ticket);
    }
    std::uint32_t epoch(std::uint64_t ticket) const {
        require_owner(); return slab_->epoch(ticket);
    }
    std::uint32_t pending(std::uint64_t ticket) const {
        require_owner(); return slab_->pending(ticket);
    }
    std::uint64_t pin(std::uint64_t ticket) {
        require_owner(); return slab_->pin(ticket);
    }
    bool settle(std::uint64_t receipt) {
        require_owner(); return slab_->settle(receipt);
    }
    bool retire(std::uint64_t ticket) {
        require_owner(); return slab_->retire(ticket);
    }
    void close() {
        require_owner();
        if (!closed_) {
            slab_->close(); // rejects unretired requests; destructor stays safe
            closed_ = true;
        }
    }

private:
    void require_owner() const {
        if (!scheduler_ || actor_budget_.retired()
                || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("proxy slab requires its exact live owner actor turn");
        }
    }

    void scrub_and_free() noexcept {
        {
            volatile std::uint32_t* raw = backing_.data();
            for (std::size_t index = 0; index < backing_.size(); ++index) {
                raw[index] = 0;
            }
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        std::vector<std::uint32_t> released;
        released.swap(backing_);
    }

    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef owner_;
    ActorShardBudget& actor_budget_;
    ActorShardBudget& vm_budget_;
    std::size_t reserved_bytes_ = 0;
    std::vector<std::uint32_t> backing_;
    std::unique_ptr<ProxyRequestSlab> slab_;
    bool closed_ = false;
};
} // namespace oreslang::runtime
