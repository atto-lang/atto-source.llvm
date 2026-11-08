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
#include "oreslang/runtime/NativeRequestLifecycleRegistryV1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
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
        virtual bool apply(RequestContextLocalV1& target, OresNativeActorScheduler* scheduler) const = 0;
    };
    template <typename T>
    struct Entry final : EntryBase {
        Entry(const RequestLocalKeyV1<T>& k, T v) : key(k), value(std::move(v)) {}
        bool apply(RequestContextLocalV1& target, OresNativeActorScheduler* scheduler) const override { return target.set(key, value, scheduler); }
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

    // Legacy host characterization is deliberately NOT the default:
    // it requires a conspicuous explicit opt-in. No guest/source compiler
    // must construct this path, because its derived contexts are untracked.
    struct UnregisteredHostOnly {
        explicit UnregisteredHostOnly() = default;
    };
    RequestContextHandoffTableV1(OresNativeActorScheduler* scheduler,
                                 std::size_t capacity,
                                 UnregisteredHostOnly)
        : scheduler_(scheduler),
          state_(std::make_shared<State>(
              capacity > max_capacity ? max_capacity : capacity)) {}

    // Trusted ingress variant: redeemed recipients are registered with the
    // exact-generation lifecycle owner BEFORE their context can be returned.
    // The registry must outlive this table and the native scheduler must
    // outlive both. A missing actor attachment fails closed.
    RequestContextHandoffTableV1(OresNativeActorScheduler* scheduler,
                                 std::size_t capacity,
                                 NativeRequestLifecycleRegistryV1& lifecycle)
        : scheduler_(scheduler), lifecycle_registry_(&lifecycle),
          state_(std::make_shared<State>(
              capacity > max_capacity ? max_capacity : capacity)) {
        // Never register a raw 'this' callback: the CONTROL reactor might
        // process STOP while the table is being destroyed. Its weak State
        // snapshot is independently refcounted and has no table pointer.
        std::weak_ptr<State> weak = state_;
        if (!lifecycle.register_stop_cleanup([weak](OresNativeActorRef actor) {
                if (auto state = weak.lock())
                    (void)revoke_state_recipient(*state, actor);
            }))
            throw std::invalid_argument(
                "handoff lifecycle cleanup registration denied");
    }
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

        // Allocate envelope outside both cancellation and scheduler locks.
        // The final table insertion is bounded: no heap allocation, no
        // guest callback and no value destruction under the lifecycle gate.
        auto prepared = std::make_unique<Envelope>(std::move(env));
        std::optional<std::uint64_t> published;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            source.cancellation_->with_active_authority([&] {
                auto commit = [&]() -> bool {
                    for (std::size_t i = 0; i < state_->slots.size(); ++i) {
                        Slot& slot = state_->slots[i];
                        if (slot.envelope) continue;
                        if (++slot.generation > kMaxGeneration)
                            slot.generation = 1;
                        slot.envelope = std::move(prepared);
                        ++state_->pending;
                        published = (slot.generation << kSlotBits) |
                                    static_cast<std::uint64_t>(i);
                        return true;
                    }
                    return false;
                };
                return ores_native_actor_scheduler_commit_if_both_live_v1(
                    scheduler_, sender, recipient,
                    [](void* p) -> int {
                        return (*static_cast<decltype(commit)*>(p))() ? 1 : 0;
                    }, &commit) != 0;
            });
        }
        return published; // no ticket if STOP or cancellation won
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
            std::lock_guard<std::mutex> lock(state_->mutex);
            const std::size_t index = static_cast<std::size_t>(ticket & kSlotMask);
            const std::uint64_t generation = ticket >> kSlotBits;
            if (index >= state_->slots.size() || generation == 0) return std::nullopt;
            Slot& s = state_->slots[index];
            if (!s.envelope || s.generation != generation) return std::nullopt;
            if (!same_actor_incarnation_v1(s.envelope->recipient, recipient))
                return std::nullopt;  // misdirected: do NOT burn it
            env = std::move(s.envelope);  // one-shot
            --state_->pending;
        }
        // Everything below runs outside the table lock (user copy/dtors).
        auto parent = env->request_cancellation.lock();
        if (!parent || parent->is_cancelled()) return std::nullopt;
        auto derived = TaskCancellation::root();
        auto link = derived->link_cancellation_from(parent);
        (void)link;  // retained by `derived` until it retires
        RequestIdentityV1 identity = env->identity;
        identity.actor = recipient;
        // The lifecycle-aware path closes the prior hole: a derived scope
        // cannot escape if the recipient actor was never attached, or STOP
        // won registry admission. On failed registration the context is
        // closed and the one-shot ticket remains consumed (no replay).
        auto context = lifecycle_registry_
            ? lifecycle_registry_->open_registered(identity, derived)
            : RequestContextLocalV1::open(identity, derived);
        RedeemedRequestContextV1 out(context, derived);
        if (!context || derived->is_cancelled() ||
            !context->bind_original_mail_once(current_mail, scheduler_)) return std::nullopt;
        for (const auto& entry : env->entries)
            if (!entry->apply(*context, scheduler_)) return std::nullopt;
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

    // CONTROL-only, automatic when constructed with a lifecycle registry.
    // Pending envelopes are detached while holding their table mutex, then
    // released outside it, so user destructors cannot deadlock the table.
    std::size_t revoke_recipient(OresNativeActorRef recipient) {
        return revoke_state_recipient(*state_, recipient);
    }

    // Drops cancelled envelopes; caller must own the host table lifetime.
    std::size_t sweep() {
        std::vector<std::unique_ptr<Envelope>> dropped;
        dropped.reserve(state_->slots.size());
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            for (auto& slot : state_->slots) {
                if (!slot.envelope) continue;
                auto parent = slot.envelope->request_cancellation.lock();
                if (!parent || parent->is_cancelled()) {
                    dropped.push_back(std::move(slot.envelope));
                    --state_->pending;
                }
            }
        }
        return dropped.size();
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->pending;
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

    struct State final {
        explicit State(std::size_t capacity) : slots(capacity) {}
        mutable std::mutex mutex;
        std::vector<Slot> slots;
        std::size_t pending = 0;
    };

    // Registry callback holds a shared State, not a table pointer. Its
    // strong lease prevents teardown UAF, and the detached vector drops
    // envelope payloads outside both registry and table mutexes.
    static std::size_t revoke_state_recipient(
            State& state, OresNativeActorRef recipient) {
        std::vector<std::unique_ptr<Envelope>> dropped;
        dropped.reserve(state.slots.size()); // allocate before table mutex
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            for (auto& slot : state.slots)
                if (slot.envelope &&
                    same_actor_incarnation_v1(
                        slot.envelope->recipient, recipient)) {
                    dropped.push_back(std::move(slot.envelope));
                    --state.pending;
                }
        }
        return dropped.size();
    }

    OresNativeActorScheduler* const scheduler_;
    NativeRequestLifecycleRegistryV1* const lifecycle_registry_ = nullptr;
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
