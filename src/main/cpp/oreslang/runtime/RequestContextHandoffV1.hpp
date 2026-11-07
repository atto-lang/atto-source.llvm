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
    using Picker = std::unique_ptr<EntryBase> (*)(const void* key, const RequestContextLocalV1&);

public:
    // Explicit list of keys to copy. Nothing is inherited implicitly.
    class Selection final {
    public:
        template <typename T>
        Selection& include(const RequestLocalKeyV1<T>& key) {
            if (keys_.size() < RequestContextLocalV1::max_slots)
                // Own the typed key identity: callers may pass temporary
                // aliases and their stack frame may end before seal().
                keys_.push_back({
                    std::static_pointer_cast<const void>(
                        std::make_shared<RequestLocalKeyV1<T>>(key)),
                    &pick<T>
                });
            else
                overflow_ = true;
            return *this;
        }
    private:
        friend class RequestContextHandoffTableV1;
        template <typename T>
        static std::unique_ptr<EntryBase> pick(const void* key, const RequestContextLocalV1& source) {
            const auto& typed = *static_cast<const RequestLocalKeyV1<T>*>(key);
            auto value = source.get_copy(typed);  // copy runs outside source mutex
            if (!value) return nullptr;
            return std::make_unique<Entry<T>>(typed, std::move(*value));
        }
        // Immutable owned key identities; never retain caller stack
        // addresses across a mailbox handoff or asynchronous selection.
        struct BoundPicker {
            std::shared_ptr<const void> key;
            Picker pick;
        };
        std::vector<BoundPicker> keys_;
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
        for (const auto& bound : selection.keys_)
            if (auto entry = bound.pick(bound.key.get(), source))
                env.entries.push_back(std::move(entry));
        // Copy constructors can run arbitrary host code, including STOP,
        // cancellation and actor turnover. Check original mail again rather
        // than accepting an unrelated later turn on the same pooled actor.
        if (!request_local_current_actor_turn_v1(
                scheduler_, sender, source)) return std::nullopt;

        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.envelope) continue;
            if (++s.generation > kMaxGeneration) s.generation = 1;
            s.envelope = std::make_unique<Envelope>(std::move(env));
            ++pending_;
            return (s.generation << kSlotBits) | static_cast<std::uint64_t>(i);
        }
        return std::nullopt;  // bounded: table full
    }

    // Recipient side. Must run on `recipient`'s authenticated turn.
    std::optional<RedeemedRequestContextV1> redeem(OresNativeActorRef recipient, std::uint64_t ticket) {
        if (!actor_turn_authenticated_v1(scheduler_, recipient)) return std::nullopt;
        // The ticket is NOT a bearer capability. Redeem only when the actual
        // scheduler-delivered DATA envelope has exactly this ticket payload,
        // not when another message on the same actor happens to know it.
        std::uint64_t current_payload = 0, current_mail = 0;
        if (!ores_native_actor_scheduler_current_data_payload(
                scheduler_, recipient, &current_payload) ||
            !ores_native_actor_scheduler_current_data_mail_sequence(
                scheduler_, recipient, &current_mail) ||
            !current_mail || current_payload != ticket) return std::nullopt;
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
        if (!context || derived->is_cancelled() ||
            !context->bind_original_mail_once(current_mail)) return std::nullopt;
        for (const auto& entry : env->entries)
            if (!entry->apply(*context)) return std::nullopt;
        // Do not publish a snapshot after STOP, actor handoff, or a
        // request-cancellation race during the selected-entry copy.
        std::uint64_t final_payload = 0, final_mail = 0;
        if (!ores_native_actor_scheduler_current_data_payload(
                scheduler_, recipient, &final_payload) ||
            !ores_native_actor_scheduler_current_data_mail_sequence(
                scheduler_, recipient, &final_mail) ||
            final_payload != ticket || final_mail != current_mail ||
            !request_local_current_actor_turn_v1(
                scheduler_, recipient, *context))
            return std::nullopt;
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
    static constexpr unsigned kSlotBits = 16;
    static constexpr std::uint64_t kSlotMask = (1u << kSlotBits) - 1;
    static constexpr std::uint64_t kMaxGeneration = (~std::uint64_t{0}) >> kSlotBits;

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
