#pragma once

// Trusted-host RequestLocal handoff across an actor boundary.
//
// Problem: actor A handles HTTP request R and delegates part of the work to
// actor B by mailbox. B needs some of R's request-local values (trace id,
// auth subject, deadline...). B must NOT get A's RequestContextLocalV1:
// sharing it would let B and A race on one mutable store, and would let B
// keep using R's storage after A's turn.
//
// Rule: what crosses the mailbox is DATA only.
//  1. On A's authenticated turn, seal() copies the explicitly selected keys
//     into an owned snapshot held by this table and returns a u64 ticket
//     {slot, generation}. The ticket is the mailbox payload. No pointer to
//     A's context, cells, or values ever leaves A.
//  2. On B's authenticated turn, redeem() checks the ticket is for exactly
//     B's actor incarnation, consumes it (one-shot), and builds a NEW
//     RequestContextLocalV1 bound to B, filled with copies.
//  3. B's context is cancelled when R is cancelled (one-way link). B
//     cancelling or closing its copy never affects R or A.
//  4. Writes on either side after seal are invisible to the other side.
//
// Not crossing: ActorLocal (belongs to one actor incarnation) and
// ThreadLocal (belongs to an OS thread) have no handoff API by design.
// TaskLocal stays within its Task; child Tasks use explicit copy_to_child.
//
// Tickets are not bearer capabilities: redeem requires the scheduler's
// current-actor claim to equal the sealed recipient. A forged, replayed,
// stale, or misdirected ticket is rejected. A misdirected redeem does not
// burn the ticket, so a wrong actor cannot destroy B's envelope.

#include "oreslang/runtime/RequestLocalSchedulerTurnV1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Deterministic, arithmetic-only ticket generation rule. The helper has no
// access to envelopes or scheduler authority; tests can exercise exhaustion
// without performing 2^48 handoffs or exposing a mutation backdoor.
namespace detail {
inline constexpr unsigned handoff_slot_bits_v1 = 16;
inline constexpr std::uint64_t handoff_max_generation_v1 =
    (~std::uint64_t{0}) >> handoff_slot_bits_v1;
inline constexpr std::optional<std::uint64_t> handoff_next_generation_v1(
    std::uint64_t previous) noexcept {
    if (previous >= handoff_max_generation_v1) return std::nullopt;
    return previous + 1;
}
} // namespace detail

// Authenticated current actor turn (same checks as the RequestLocal adapter,
// without requiring a request bound to that actor).
inline bool actor_turn_authenticated_v1(OresNativeActorScheduler* scheduler,
                                        OresNativeActorRef actor) {
    if (!scheduler || !actor.scheduler_domain || !actor.generation ||
        !ores_native_actor_scheduler_is_current_actor(scheduler, actor))
        return false;
    OresNativeExecutionContext context{};
    if (!ores_native_actor_scheduler_current_execution_context(scheduler, &context))
        return false;
    return context.id.runtime_domain == actor.scheduler_domain &&
        context.id.execution != 0 &&
        context.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR &&
        context.allocation.domain == actor.scheduler_domain &&
        context.allocation.owner == actor.actor_slot &&
        context.allocation.generation == actor.generation;
}

inline bool same_actor_incarnation_v1(OresNativeActorRef a, OresNativeActorRef b) noexcept {
    return a.scheduler_domain == b.scheduler_domain &&
        a.actor_slot == b.actor_slot && a.generation == b.generation;
}

// Owned by the recipient. Closing it closes the derived context and retires
// its cancellation node; the original request is unaffected.
class RedeemedRequestContextV1 final {
public:
    RedeemedRequestContextV1(RedeemedRequestContextV1&& other) noexcept
        : context_(std::move(other.context_)), cancellation_(std::move(other.cancellation_)) {}
    RedeemedRequestContextV1& operator=(RedeemedRequestContextV1&&) = delete;
    RedeemedRequestContextV1(const RedeemedRequestContextV1&) = delete;
    RedeemedRequestContextV1& operator=(const RedeemedRequestContextV1&) = delete;
    ~RedeemedRequestContextV1() { finish(); }

    RequestContextLocalV1& context() const noexcept { return *context_; }
    // Recipient-local cancellation (e.g. B gives up). Does not reach R.
    bool cancel_local(std::string reason = "recipient-cancelled") {
        return cancellation_ && cancellation_->cancel(std::move(reason));
    }
    void finish() noexcept {
        if (context_) context_->close();
        context_.reset();
        if (cancellation_) {
            try { cancellation_->retire(); } catch (...) {}
        }
        cancellation_.reset();
    }

private:
    friend class RequestContextHandoffTableV1;
    RedeemedRequestContextV1(RequestContextLocalV1::Ptr context, TaskCancellation::Ptr cancellation)
        : context_(std::move(context)), cancellation_(std::move(cancellation)) {}
    RequestContextLocalV1::Ptr context_;
    TaskCancellation::Ptr cancellation_;
};

class RequestContextHandoffTableV1 final {
    struct EntryBase {
        virtual ~EntryBase() = default;
        virtual bool apply(RequestContextLocalV1& target) const = 0;
    };
    template <typename T>
    struct Entry final : EntryBase {
        Entry(const RequestLocalKeyV1<T>& k, T v) : key(k), value(std::move(v)) {}
        bool apply(RequestContextLocalV1& target) const override { return target.set(key, value); }
        RequestLocalKeyV1<T> key;
        T value;
    };
public:
    // Explicit list of keys to copy. Nothing is inherited implicitly.
    class Selection final {
    public:
        template <typename T>
        Selection& include(const RequestLocalKeyV1<T>& key) {
            // Store the minted key by VALUE. A selection may outlive the
            // expression or stack frame that supplied this key; retaining
            // &key here would cause use-after-scope during seal().
            if (keys_.size() < RequestContextLocalV1::max_slots)
                keys_.push_back(std::make_shared<KeyPick<T>>(key));
            else
                overflow_ = true;
            return *this;
        }
    private:
        friend class RequestContextHandoffTableV1;
        struct KeyPickBase {
            virtual ~KeyPickBase() = default;
            virtual std::unique_ptr<EntryBase> pick(
                const RequestContextLocalV1& source) const = 0;
        };
        template <typename T>
        struct KeyPick final : KeyPickBase {
            explicit KeyPick(const RequestLocalKeyV1<T>& input) : key(input) {}
            std::unique_ptr<EntryBase> pick(
                const RequestContextLocalV1& source) const override {
                auto value = source.get_copy(key); // copy outside source mutex
                if (!value) return nullptr;
                return std::make_unique<Entry<T>>(key, std::move(*value));
            }
            const RequestLocalKeyV1<T> key;
        };
        std::vector<std::shared_ptr<const KeyPickBase>> keys_;
        bool overflow_ = false;
    };

    static constexpr std::size_t max_capacity = 1u << 16;

    RequestContextHandoffTableV1(OresNativeActorScheduler* scheduler, std::size_t capacity)
        : scheduler_(scheduler), slots_(capacity > max_capacity ? max_capacity : capacity) {}
    RequestContextHandoffTableV1(const RequestContextHandoffTableV1&) = delete;
    RequestContextHandoffTableV1& operator=(const RequestContextHandoffTableV1&) = delete;

    // Sender side. Must run on `sender`'s authenticated turn, and `source`
    // must belong to `sender` and be active.
    std::optional<std::uint64_t> seal(OresNativeActorRef sender,
                                      const RequestContextLocalV1& source,
                                      OresNativeActorRef recipient,
                                      const Selection& selection) {
        if (selection.overflow_ || !recipient.generation ||
            recipient.scheduler_domain != sender.scheduler_domain ||
            !request_local_current_actor_turn_v1(scheduler_, sender, source))
            return std::nullopt;
        Envelope env;
        env.identity = source.identity();
        env.recipient = recipient;
        env.request_cancellation = source.cancellation_;
        env.entries.reserve(selection.keys_.size());
        for (const auto& pick : selection.keys_)
            if (auto entry = pick->pick(source)) env.entries.push_back(std::move(entry));
        // Re-check after user copy constructors ran: the request may have
        // been cancelled meanwhile. Never hand off a dead request.
        if (!source.active()) return std::nullopt;

        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.envelope) continue;
            // Never recycle a ticket generation: wrapping would eventually
            // let an ancient mailbox payload authorize a different envelope.
            // Exhausted slots are permanently retired instead.
            const auto next_generation = detail::handoff_next_generation_v1(s.generation);
            if (!next_generation) continue;
            s.generation = *next_generation;
            s.envelope = std::make_unique<Envelope>(std::move(env));
            ++pending_;
            return (s.generation << kSlotBits) | static_cast<std::uint64_t>(i);
        }
        return std::nullopt;  // bounded: table full
    }

    // Recipient side. Must run on `recipient`'s authenticated turn.
    std::optional<RedeemedRequestContextV1> redeem(OresNativeActorRef recipient, std::uint64_t ticket) {
        if (!actor_turn_authenticated_v1(scheduler_, recipient)) return std::nullopt;
        std::unique_ptr<Envelope> env;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t index = static_cast<std::size_t>(ticket & kSlotMask);
            const std::uint64_t generation = ticket >> kSlotBits;
            if (index >= slots_.size() || generation == 0) return std::nullopt;
            Slot& s = slots_[index];
            if (!s.envelope || s.generation != generation) return std::nullopt;
            if (!same_actor_incarnation_v1(s.envelope->recipient, recipient))
                return std::nullopt;  // misdirected: do NOT burn it
            env = std::move(s.envelope);  // one-shot
            --pending_;
        }
        // Everything below runs outside the table lock (user copy/dtors).
        auto parent = env->request_cancellation.lock();
        if (!parent || parent->is_cancelled()) return std::nullopt;
        auto derived = TaskCancellation::root();
        auto link = derived->link_cancellation_from(parent);
        (void)link;  // retained by `derived` until it retires
        RequestIdentityV1 identity = env->identity;
        identity.actor = recipient;
        auto context = RequestContextLocalV1::open(identity, derived);
        RedeemedRequestContextV1 out(context, derived);
        if (!context || derived->is_cancelled()) return std::nullopt;
        for (const auto& entry : env->entries)
            if (!entry->apply(*context)) return std::nullopt;  // cancelled mid-copy
        return out;
    }

    // Scheduler calls this when an actor incarnation ends: envelopes sealed
    // for it can never be redeemed and are dropped now.
    std::size_t revoke_recipient(OresNativeActorRef recipient) {
        std::vector<std::unique_ptr<Envelope>> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& s : slots_)
                if (s.envelope && same_actor_incarnation_v1(s.envelope->recipient, recipient)) {
                    dropped.push_back(std::move(s.envelope));
                    --pending_;
                }
        }
        return dropped.size();  // values destroyed outside the lock
    }

    // Drops envelopes whose request has ended or been cancelled.
    std::size_t sweep() {
        std::vector<std::unique_ptr<Envelope>> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& s : slots_) {
                if (!s.envelope) continue;
                auto parent = s.envelope->request_cancellation.lock();
                if (!parent || parent->is_cancelled()) {
                    dropped.push_back(std::move(s.envelope));
                    --pending_;
                }
            }
        }
        return dropped.size();
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

private:
    static constexpr unsigned kSlotBits = detail::handoff_slot_bits_v1;
    static constexpr std::uint64_t kSlotMask = (1u << kSlotBits) - 1;

    struct Envelope {
        RequestIdentityV1 identity{};
        OresNativeActorRef recipient{};
        // Weak: a pending envelope never keeps a finished request alive.
        std::weak_ptr<TaskCancellation> request_cancellation;
        std::vector<std::unique_ptr<EntryBase>> entries;
    };
    struct Slot {
        std::uint64_t generation = 0;
        std::unique_ptr<Envelope> envelope;
    };

    OresNativeActorScheduler* const scheduler_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::size_t pending_ = 0;
};

} // namespace oreslang::runtime
