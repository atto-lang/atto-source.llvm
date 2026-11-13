#pragma once

#include "oreslang/runtime/NativeFixedBufferPool.hpp"
#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Preallocated HOST data lane for transferable pool buffers; NOT an actor
// scheduler, CONTROL lane, authenticated guest capability, or I/O pin. A
// trusted actor turn and resource-registry lifecycle fence are still required.
// Credits stay charged until explicit physical release, not dequeue.
class NativeFixedBufferMailbox final {
    using Lease = NativeFixedBufferPool::Lease;
    struct State {
        explicit State(std::size_t slots, BufferActorIdentity receiver,
                       std::shared_ptr<ActorResourceRegistry> registry)
            : target_actor(receiver), ring(slots), receiver_loans(slots),
              receiver_registry(std::move(registry)) {}
        std::mutex mutex;
        const BufferActorIdentity target_actor;
        std::vector<std::optional<Lease>> ring;
        std::vector<std::optional<ActorResourceRegistry::Ticket>> receiver_loans;
        // Holds the physical receiver registry until every delivery is
        // settled; registry tickets independently block actor reclaim.
        std::shared_ptr<ActorResourceRegistry> receiver_registry;
        std::size_t head = 0, tail = 0, queued = 0, charged = 0;
        bool retired = false;
    };

public:
    class Delivery final {
    public:
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&&) noexcept = default;
        Delivery& operator=(Delivery&&) = delete;
        ~Delivery() = default;

        Lease& lease() {
            if (!owned_ || !state_) throw std::logic_error("stale buffer delivery");
            return *owned_;
        }
        bool acknowledge_released() noexcept {
            if (!owned_ || !state_ || !owned_->release()) return false;
            owned_.reset();
            // Physical payload storage is released BEFORE loan revocation.
            // Never allow a second ACK to refund a frame twice.
            if (receiver_loan_) {
                try {
                    state_->receiver_registry->release_loan(*receiver_loan_);
                } catch (...) {
                    std::terminate(); // invalid receipt must fail closed
                }
                receiver_loan_.reset();
            }
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->charged == 0) std::terminate();
            --state_->charged;
            return true;
        }

    private:
        friend class NativeFixedBufferMailbox;
        Delivery(std::shared_ptr<State> state, Lease&& lease,
                 std::optional<ActorResourceRegistry::Ticket> loan) noexcept
            : state_(std::move(state)), owned_(std::move(lease)),
              receiver_loan_(std::move(loan)) {}
        std::shared_ptr<State> state_;
        std::optional<Lease> owned_;
        // If a delivery dies without ACK, never mint a release or return
        // frame credit; the issued registry loan also remains outstanding.
        std::optional<ActorResourceRegistry::Ticket> receiver_loan_;
    };

    NativeFixedBufferMailbox(
        std::size_t slots, BufferActorIdentity target_actor,
        std::shared_ptr<ActorResourceRegistry> receiver_registry = {}) {
        if (slots == 0 || !target_actor || slots > (1u << 20) ||
            (receiver_registry &&
             (receiver_registry->actor_id() != target_actor.actor_id ||
              receiver_registry->incarnation() != target_actor.generation)))
            throw std::invalid_argument("invalid native buffer mailbox size/target");
        state_ = std::make_shared<State>(
            slots, target_actor, std::move(receiver_registry));
    }
    NativeFixedBufferMailbox(const NativeFixedBufferMailbox&) = delete;
    NativeFixedBufferMailbox& operator=(const NativeFixedBufferMailbox&) = delete;
    ~NativeFixedBufferMailbox() { retire(); }

    // Unbound legacy host-only port, with no resource-registry authority or
    // shutdown/fence evidence. Denied when a physical registry is bound.
    bool try_offer(NativeFixedBufferPool& pool, Lease& sender,
                   BufferActorIdentity source_actor) noexcept {
        if (state_->receiver_registry) return false;
        return try_offer_impl(pool, sender, source_actor, std::nullopt);
    }

    // Scheduler-only admission: both source and destination incarnations
    // have independently minted resource tickets BEFORE publication. An
    // accepted delivery holds its receiver loan until physical ACK, blocking
    // actor-resource reclamation while queued or checked out.
    //
    // Actor and tenant identity MUST be authenticated before this entry; the
    // BufferActorIdentity value does not grant authorization on its own.
    // Note that ActorResourceRegistry's unordered_set token admission may
    // allocate: this is NOT a zero-allocation guest hot-path proof.
    bool try_offer_authenticated(
        NativeFixedBufferPool& pool, Lease& sender,
        BufferActorIdentity source_actor,
        ActorResourceRegistry& source_registry) noexcept {
        const auto receiver_registry = state_->receiver_registry;
        if (!receiver_registry || !source_actor ||
            source_actor.actor_id != source_registry.actor_id() ||
            source_actor.generation != source_registry.incarnation())
            return false;
        std::optional<ActorResourceRegistry::Ticket> handoff, loan;
        try {
            handoff.emplace(source_registry.acquire_handoff());
            loan.emplace(receiver_registry->acquire_loan());
        } catch (...) {
            if (handoff) {
                try { source_registry.release_handoff(*handoff); }
                catch (...) { std::terminate(); }
            }
            return false; // no sender lease or mailbox accounting moved
        }

        const bool accepted = try_offer_impl(pool, sender, source_actor, loan);
        try {
            // Sender handoff settles only after commit or exact rollback.
            source_registry.release_handoff(*handoff);
            // Failed mailbox admission must return receiver admission loan.
            if (!accepted) receiver_registry->release_loan(*loan);
        } catch (...) {
            std::terminate(); // no forgery of a missing physical receipt
        }
        return accepted;
    }

    std::optional<Delivery> try_receive() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->queued == 0) return std::nullopt;
        auto lease = std::move(*state_->ring[state_->head]);
        auto loan = std::move(state_->receiver_loans[state_->head]);
        state_->ring[state_->head].reset();
        state_->receiver_loans[state_->head].reset();
        state_->head = (state_->head + 1) % state_->ring.size();
        --state_->queued;
        return Delivery(state_, std::move(lease), std::move(loan));
    }

    // Seal new sends, but allow receiver to settle already charged deliveries.
    // False means the scheduler MUST retain this mailbox until quiescence.
    bool retire() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->retired = true;
        return state_->charged == 0;
    }
    std::size_t charged() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->charged;
    }
    std::size_t queued() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->queued;
    }

private:
    bool try_offer_impl(
        NativeFixedBufferPool& pool, Lease& sender,
        BufferActorIdentity source_actor,
        std::optional<ActorResourceRegistry::Ticket> receiver_loan) noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->retired || state_->charged == state_->ring.size() ||
            !source_actor || source_actor.tenant_id != state_->target_actor.tenant_id)
            return false;
        auto prepared = pool.try_prepare_transfer(sender, source_actor);
        if (!prepared) return false;
        auto delivered = prepared->commit(state_->target_actor);
        if (!delivered) {
            auto restored = prepared->abort();
            if (restored) sender = std::move(*restored);
            // No partial publish; a failed rollback's escrow destructor
            // quarantines the payload by revoking it.
            return false;
        }
        state_->ring[state_->tail].emplace(std::move(*delivered));
        state_->receiver_loans[state_->tail] = std::move(receiver_loan);
        state_->tail = (state_->tail + 1) % state_->ring.size();
        ++state_->queued;
        ++state_->charged;
        return true;
    }
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
