#pragma once

#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <mutex>
#include <memory>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Host-runtime-only, independently bounded URGENT/NORMAL/BULK user inbox.
 *
 * This is a transport/scheduler building block, NOT a guest-visible priority
 * send capability. Compiler lowering must authenticate who may use URGENT.
 * Privileged supervisor STOP/CANCEL/PANIC delivery is separately pre-reserved
 * in SupervisorControlTurn; it must be polled before this inbox at each
 * authorized logical actor turn, never deposited in any user port.
 *
 * Every lane charges queued, preparing, AND checked-out bytes/frames until
 * the receiver acknowledges physical quiescence. Never run a user callback
 * under the fairness lock. One actor turn still belongs to its physical gate.
 */
template <typename T>
class PriorityActorInbox final {
public:
    enum class Lane { Urgent, Normal, Bulk };
    enum class Dispatch { Control, User, Idle, Busy, Retired };
    struct Limits {
        std::size_t frames;
        std::size_t bytes;
    };
    using OwnedDelivery = typename BoundedActorPort<T>::Delivery;
    using SelectiveCursor = typename BoundedActorPort<T>::SelectiveCursor;
    using SelectiveStatus = typename BoundedActorPort<T>::SelectiveStatus;

    class Delivery final {
    public:
        Delivery(Lane lane, OwnedDelivery&& owned,
                 std::shared_ptr<ActorMailboxCompletion> completion,
                 std::optional<NativeActorRef> actor)
            : lane_(lane), owned_(std::move(owned)),
              completion_(std::move(completion)),
              bound_actor_(std::move(actor)) {}
        Delivery(Delivery&&) noexcept = default;
        Delivery& operator=(Delivery&&) noexcept = delete;
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;

        Lane lane() const noexcept { return lane_; }
        T& value() { return owned_.value(); }
        const T& value() const { return owned_.value(); }
        std::size_t charged_bytes() const noexcept { return owned_.charged_bytes(); }
        std::uint64_t sequence() const noexcept { return owned_.sequence(); }
        bool acknowledge_released() {
            if (!owned_.acknowledge_released()) return false;
            // Credit is refunded only after physical quiescence. Record the
            // sequence after ACK, so an earlier message cannot be skipped.
            completion_->complete(owned_.sequence());
            if (bound_actor_) bound_actor_->release_resource_loan();
            return true;
        }

        // Only runtime-owned terminal STOP/CANCEL cleanup calls this after
        // quiescence. A discarded delivery is not a processed message:
        // its sequence advances the contiguous fence but fails drain_status.
        bool retire_aborted_after_quiescence() {
            if (!owned_.acknowledge_released()) return false;
            completion_->complete(owned_.sequence(), true);
            if (bound_actor_) bound_actor_->release_resource_loan();
            return true;
        }
    private:
        Lane lane_;
        OwnedDelivery owned_;
        std::shared_ptr<ActorMailboxCompletion> completion_;
        std::optional<NativeActorRef> bound_actor_;
    };

    PriorityActorInbox(Limits urgent, Limits normal, Limits bulk,
                       std::size_t max_unfinished = 4096)
        : completion_(std::make_shared<ActorMailboxCompletion>(max_unfinished)),
          urgent_(urgent.frames, urgent.bytes, completion_),
          normal_(normal.frames, normal.bytes, completion_),
          bulk_(bulk.frames, bulk.bytes, completion_) {}

    PriorityActorInbox(const PriorityActorInbox&) = delete;
    PriorityActorInbox& operator=(const PriorityActorInbox&) = delete;

    /**
     * Bind this inbox exactly once to the actor incarnation. The generation
     * cannot be rotated on a live inbox; restarting an actor requires a new
     * inbox, with the old generation retired and its leases quiesced.
     */
    void bind_target(const NativeActorRef& target) {
        std::lock_guard<std::mutex> guard(service_mutex_);
        if (sealed_ || bound_target_ || active_admissions_ != 0 ||
            completion_->snapshot() != 0)
            throw std::logic_error("actor inbox target already bound or active");
        bound_actor_ = target;
        bound_target_ = OwnerKey{target.runtime_id(), target.actor_id(),
                                 target.generation()};
    }

    // Runtime entry point for generation-fenced enqueue; the issuer's rights
    // to choose URGENT still require separate capability verification.
    bool offer_to(const NativeActorRef& target, Lane lane, T&& value,
                  std::size_t bytes, std::uint32_t match_tag = 0) {
        AdmissionGuard admission(*this, &target);
        const bool accepted = port(lane).offer(std::move(value), bytes, match_tag);
        if (accepted) admission.commit();
        return accepted;
    }

    // Called only by trusted admission after source/role/priority validation.
    // Failed offers leave the producer's T untouched and do not queue a waiter.
    bool offer(Lane lane, T&& value, std::size_t bytes,
               std::uint32_t match_tag = 0) {
        AdmissionGuard admission(*this, nullptr);
        return port(lane).offer(std::move(value), bytes, match_tag);
    }

    /**
     * Prefer this over offer(T&&) for exclusive kernel/arena/FFI handles.
     * The prebuilt unique owner transfers only after publication succeeds.
     * Authority for URGENT still belongs to the trusted runtime caller.
     */
    bool offer_owned(Lane lane, std::unique_ptr<T>& value, std::size_t bytes,
                     std::uint32_t match_tag = 0) {
        AdmissionGuard admission(*this, nullptr);
        return port(lane).offer_owned(value, bytes, match_tag);
    }

    bool offer_owned_to(const NativeActorRef& target, Lane lane,
                        std::unique_ptr<T>& value, std::size_t bytes,
                        std::uint32_t match_tag = 0) {
        AdmissionGuard admission(*this, &target);
        const bool accepted = port(lane).offer_owned(value, bytes, match_tag);
        if (accepted) admission.commit();
        return accepted;
    }

    /**
     * One fair dequeue opportunity. The 8:4:1 static weighted service cycle
     * is work-conserving: empty lanes are skipped; live NORMAL/BULK traffic
     * cannot be starved by continuously refilled URGENT traffic. Every lane
     * remains FIFO. This does NOT grant a concurrent actor execution turn.
     */
    std::optional<Delivery> try_receive() {
        std::lock_guard<std::mutex> guard(service_mutex_);
        if (bound_target_)
            throw std::logic_error("bound actor mailbox dequeue requires authorized actor turn");
        return try_receive_locked();
    }

private:
    // Caller must hold service_mutex_ and have either unbound transport
    // authority or an actor/retirement turn authorized by NativeActorGroup.
    std::optional<Delivery> try_receive_locked() {
        for (std::size_t checked = 0; checked < service_order_.size(); ++checked) {
            const auto lane = service_order_[next_slot_];
            next_slot_ = (next_slot_ + 1) % service_order_.size();
            auto receipt = port(lane).try_receive();
            if (receipt)
                return Delivery(lane, std::move(*receipt), completion_, bound_actor_);
        }
        return std::nullopt;
    }

public:
    // Tagged stage-1 selective receive is lane-LOCAL. Existing weighted
    // inter-lane priority scheduling still governs ordinary try_receive(),
    // so a full Erlang single-global-mailbox selective receive is NOT
    // implemented by this entry point. Only a compiler/runtime-generated
    // immutable tag may be supplied; no user callbacks execute under locks.
    //
    // For a bound actor, the trusted scheduler MUST call this only from an
    // authorized single-flight user turn, having polled priority CONTROL.
    // Any Continue cursor must be retained only by that actor incarnation,
    // with the same lane, tag and selector; always yield on scan exhaustion.
    struct SelectiveResult {
        SelectiveStatus status;
        std::optional<Delivery> delivery;
        std::size_t inspected;
        SelectiveCursor cursor;
    };
    // Unbound transport test/probe API only. A bound actor must never
    // consume typed user messages outside its authenticated CONTROL-first
    // single-flight actor turn; use dispatch_next_tagged instead.
    SelectiveResult try_receive_tagged(
            Lane lane, std::uint32_t tag, std::size_t scan_budget,
            const SelectiveCursor& cursor = {}) {
        std::lock_guard<std::mutex> guard(service_mutex_);
        if (bound_target_)
            throw std::logic_error("bound actor selective receive requires authorized actor turn");
        return try_receive_tagged_locked(lane, tag, scan_budget, cursor);
    }

private:
    // Requires service_mutex_. Caller owns either the explicitly unbound
    // probe or a generation-checked, authorized actor user turn.
    SelectiveResult try_receive_tagged_locked(
            Lane lane, std::uint32_t tag, std::size_t scan_budget,
            const SelectiveCursor& cursor) {
        auto result = port(lane).try_receive_tagged(tag, scan_budget, cursor);
        std::optional<Delivery> owned;
        if (result.delivery)
            owned.emplace(lane, std::move(*result.delivery), completion_, bound_actor_);
        return {result.status, std::move(owned), result.inspected,
                std::move(result.cursor)};
    }
public:

    /**
     * Actual native host actor-turn integration. The group, not the mailbox,
     * decides whether a logical turn is admitted, and CONTROL is polled first.
     * Work is dequeued ONLY from inside a granted single-flight user turn.
     * A raced CONTROL enqueue makes run_user_turn() return Busy instead.
     *
     * process(receipt) must acknowledge physical release or move the receipt
     * to its own tracked completion path. Dropping an unacknowledged receipt
     * remains fail-closed and deliberately leaks the credit pending teardown.
     * Guest compiler/runtime dispatch remains a separate integration gate.
     */
    template<class Process>
    Dispatch dispatch_next(NativeActorGroup& group, const NativeActorRef& actor,
                           Process&& process) {
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            if (!bound_target_ || !same_target(actor)) return Dispatch::Retired;
        }
        switch (group.run_control_turn(actor)) {
            case NativeActorGroup::ControlTurn::executed: return Dispatch::Control;
            case NativeActorGroup::ControlTurn::busy: return Dispatch::Busy;
            case NativeActorGroup::ControlTurn::retired: return Dispatch::Retired;
            case NativeActorGroup::ControlTurn::idle: break;
        }
        bool consumed = false;
        const auto outcome = group.run_user_turn(actor, [&] {
            auto next = [&]() {
                std::lock_guard<std::mutex> guard(service_mutex_);
                if (!bound_target_ || !same_target(actor))
                    throw std::logic_error("actor changed during user mailbox turn");
                return try_receive_locked();
            }();
            if (!next) return;
            consumed = true;
            process(*next);
        });
        if (outcome == NativeActorGroup::UserTurn::retired) return Dispatch::Retired;
        if (outcome == NativeActorGroup::UserTurn::busy) return Dispatch::Busy;
        return consumed ? Dispatch::User : Dispatch::Idle;
    }

    // The scheduling-aware entry point for compiler-generated typed tag
    // dispatch. Unlike the low-level selector, this proves the actor's
    // incarnation is still bound, polls privileged CONTROL first, and only
    // scans within an admitted nonreentrant actor turn. A budget hit yields
    // a resumable cursor rather than parking an actor carrier.
    //
    // This stage remains lane-local; global Erlang order, typed payload guards,
    // deadline arbitration and guest AOT emission are separate gates.
    enum class SelectiveDispatch { Control, User, Idle, Busy, Retired, Resume };
    template<class Process>
    SelectiveDispatch dispatch_next_tagged(
            NativeActorGroup& group, const NativeActorRef& actor,
            Lane lane, std::uint32_t tag, std::size_t scan_budget,
            SelectiveCursor& cursor, Process&& process) {
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            if (!bound_target_ || !same_target(actor))
                return SelectiveDispatch::Retired;
        }
        switch (group.run_control_turn(actor)) {
            case NativeActorGroup::ControlTurn::executed: return SelectiveDispatch::Control;
            case NativeActorGroup::ControlTurn::busy: return SelectiveDispatch::Busy;
            case NativeActorGroup::ControlTurn::retired: return SelectiveDispatch::Retired;
            case NativeActorGroup::ControlTurn::idle: break;
        }
        bool matched = false, resume = false;
        const auto outcome = group.run_user_turn(actor, [&] {
            // The current user turn is already granted by NativeActorGroup;
            // recheck the bound identity before entering the raw port path.
            SelectiveResult result = [&] {
                std::lock_guard<std::mutex> guard(service_mutex_);
                if (!bound_target_ || !same_target(actor))
                    throw std::logic_error("actor changed during selective user turn");
                return try_receive_tagged_locked(lane, tag, scan_budget, cursor);
            }();
            cursor = std::move(result.cursor);
            if (result.status == SelectiveStatus::Continue) {
                resume = true;
                return;
            }
            if (!result.delivery) return;
            matched = true;
            process(*result.delivery);
        });
        if (outcome == NativeActorGroup::UserTurn::retired) return SelectiveDispatch::Retired;
        if (outcome == NativeActorGroup::UserTurn::busy) return SelectiveDispatch::Busy;
        if (resume) return SelectiveDispatch::Resume;
        return matched ? SelectiveDispatch::User : SelectiveDispatch::Idle;
    }

    std::size_t used_frames(Lane lane) const { return port(lane).used_frames(); }
    std::size_t used_bytes(Lane lane) const { return port(lane).used_bytes(); }

    // A drain captures snapshot() at its linearization point. An overtaking
    // URGENT may ACK before an earlier NORMAL, but it cannot advance this
    // contiguous watermark across that gap. No waiter is registered here.
    std::uint64_t snapshot() const { return completion_->snapshot(); }
    std::uint64_t watermark() const { return completion_->watermark(); }
    bool completed_through(std::uint64_t target) const {
        return completion_->completed_through(target);
    }
    using DrainStatus = ActorMailboxCompletion::DrainStatus;
    DrainStatus drain_status(std::uint64_t target) const {
        return completion_->drain_status(target);
    }

    /**
     * Terminal STOP/CANCEL cleanup after sender preparations and other actor
     * carriers have been externally fenced. All queued user mail is retired
     * as ABORTED (never misreported as successfully delivered), while any
     * already checked-out in-flight delivery remains owned by its receiver.
     *
     * Call only after actor scheduler/lifecycle grants terminal cleanup
     * authority. A pending producer move can still publish an admitted
     * frame after seal; callers MUST join those moves before this method.
     */
    // For a bound actor this is the ONLY retirement entry point. A numeric
    // generation match is insufficient: the exact NativeActorRef and
    // RuntimeFinalizer capability must come from the owning closed group.
    std::size_t retire_queued_after_quiescence(
            const NativeActorGroup::RuntimeFinalizer& finalizer,
            const NativeActorRef& actor) {
        if (!finalizer.matches(actor) ||
            !finalizer.ready_for_actor_turn_quiescence())
            throw std::logic_error("actor mailbox retirement lacks physical turn fence");
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            if (!bound_target_ || !same_target(actor))
                throw std::logic_error("actor mailbox retirement wrong generation");
            sealed_ = true;
        }
        return retire_queued_impl();
    }

    // Legacy host-only helper for unbound native test/transport mailboxes.
    // Bound actor mailboxes REQUIRE the real group finalizer capability.
    std::size_t retire_queued_after_quiescence() {
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            if (bound_target_)
                throw std::logic_error("bound actor mailbox requires finalizer fence");
            // Prevent a new bind/send from racing the transition to terminal
            // cleanup even if the inbox has never published a message.
            sealed_ = true;
        }
        return retire_queued_impl();
    }

private:
    std::size_t retire_queued_impl() {
        try { close(); }
        catch (const std::logic_error&) {
            // Expected: outstanding queued/checked-out/preparing credits;
            // every lane has already been sealed by close().
        }
        // A sender may pass the inbox gate but be descheduled before it
        // enters a port. The port's pending_prepares() is then still zero.
        // Seal first, and require the outer admission guard to have exited.
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            if (active_admissions_ != 0)
                throw std::logic_error(
                    "actor mailbox terminal retirement requires admission quiescence");
        }
        // Closing admits no *new* sends, but an already reserved producer
        // can still publish after close(). Returning successfully now would
        // orphan that envelope and leave both mailbox credits and drain
        // completion forever unsettled. Require an externally observed
        // preparation fence; never park an actor scheduler carrier here.
        if (urgent_.pending_prepares() != 0 ||
            normal_.pending_prepares() != 0 ||
            bulk_.pending_prepares() != 0)
            throw std::logic_error(
                "actor mailbox terminal retirement requires producer quiescence");
        std::size_t count = 0;
        for (;;) {
            auto next = [&]() {
                std::lock_guard<std::mutex> guard(service_mutex_);
                return try_receive_locked();
            }();
            if (!next) break;
            if (!next->retire_aborted_after_quiescence())
                throw std::logic_error("actor mailbox retirement duplicate receipt");
            ++count;
        }
        return count;
    }

public:
    // Seal all lanes even if the first lane still has outstanding credits.
    // Previously admitted messages remain drainable after close.
    void close() {
        {
            std::lock_guard<std::mutex> guard(service_mutex_);
            sealed_ = true; // publishing barrier for bind and new admission
        }
        std::exception_ptr first;
        auto seal = [&first](BoundedActorPort<T>& lane) {
            try { lane.close(); }
            catch (...) { if (!first) first = std::current_exception(); }
        };
        seal(urgent_);
        seal(normal_);
        seal(bulk_);
        if (first) std::rethrow_exception(first);
    }

private:
    // Prevent a send started before bind_target() from publishing into a
    // newly bound actor. Drop the fairness lock BEFORE moving a payload: its
    // user-supplied move constructor may re-enter the inbox. RAII makes all
    // rejection/exception/close paths release the transient admission fence.
    struct AdmissionGuard final {
        PriorityActorInbox& inbox;
        bool retained = false;
        bool committed = false;
        AdmissionGuard(PriorityActorInbox& inbox, const NativeActorRef* target)
            : inbox(inbox) {
            std::lock_guard<std::mutex> guard(inbox.service_mutex_);
            if (inbox.sealed_)
                throw std::logic_error("sealed actor inbox rejects new sends");
            if (target) {
                if (!inbox.bound_target_ || !inbox.same_target(*target))
                    throw std::logic_error("stale or foreign actor mailbox target");
                inbox.bound_actor_->retain_resource_loan();
                retained = true;
            } else if (inbox.bound_target_) {
                throw std::logic_error("bound actor inbox requires target admission");
            }
            ++inbox.active_admissions_;
        }
        void commit() noexcept { committed = true; }
        ~AdmissionGuard() noexcept {
            std::lock_guard<std::mutex> guard(inbox.service_mutex_);
            if (!inbox.active_admissions_) std::terminate();
            --inbox.active_admissions_;
            if (retained && !committed)
                inbox.bound_actor_->release_resource_loan();
        }
        AdmissionGuard(const AdmissionGuard&) = delete;
        AdmissionGuard& operator=(const AdmissionGuard&) = delete;
    };

    struct OwnerKey {
        std::uint64_t runtime;
        std::uint64_t actor;
        std::uint64_t generation;
    };

    bool same_target(const NativeActorRef& ref) const noexcept {
        return bound_target_ && bound_actor_
            && bound_target_->runtime == ref.runtime_id()
            && bound_target_->actor == ref.actor_id()
            && bound_target_->generation == ref.generation()
            && bound_actor_->same_incarnation(ref);
    }

    BoundedActorPort<T>& port(Lane lane) {
        switch (lane) {
            case Lane::Urgent: return urgent_;
            case Lane::Normal: return normal_;
            case Lane::Bulk: return bulk_;
        }
        throw std::invalid_argument("forged actor message priority");
    }

    const BoundedActorPort<T>& port(Lane lane) const {
        return const_cast<PriorityActorInbox*>(this)->port(lane);
    }

    inline static constexpr std::array<Lane, 13> service_order_{
        Lane::Urgent, Lane::Urgent, Lane::Urgent, Lane::Urgent,
        Lane::Urgent, Lane::Urgent, Lane::Urgent, Lane::Urgent,
        Lane::Normal, Lane::Normal, Lane::Normal, Lane::Normal,
        Lane::Bulk
    };

    std::shared_ptr<ActorMailboxCompletion> completion_;
    BoundedActorPort<T> urgent_;
    BoundedActorPort<T> normal_;
    BoundedActorPort<T> bulk_;
    std::mutex service_mutex_;
    std::optional<OwnerKey> bound_target_;
    std::optional<NativeActorRef> bound_actor_;
    bool sealed_ = false;
    std::size_t active_admissions_ = 0;
    std::size_t next_slot_ = 0;
};

} // namespace oreslang::runtime
