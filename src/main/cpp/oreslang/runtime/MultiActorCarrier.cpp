#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {
namespace {
struct ActiveGuestTurn {
    ActorGuestFrameStack* stack = nullptr;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    ActorGuestFrameStack::FrameToken generated_scalar{};
};
thread_local ActiveGuestTurn active_guest_turn{};
class ScopedGuestTurn final {
    ActiveGuestTurn previous_;
public:
    ScopedGuestTurn(ActorGuestFrameStack* stack, std::uint32_t slot,
                    std::uint64_t generation) noexcept
        : previous_(active_guest_turn) {
        active_guest_turn = {stack, slot, generation, {}};
    }
    ~ScopedGuestTurn() noexcept { active_guest_turn = previous_; }
    ScopedGuestTurn(const ScopedGuestTurn&) = delete;
    ScopedGuestTurn& operator=(const ScopedGuestTurn&) = delete;
};
ActorGuestFrameStack* authenticated_stack(MultiActorCarrier::ActorToken token) noexcept {
    if (token.slot == 0 || token.generation == 0 ||
            token.slot != active_guest_turn.slot ||
            token.generation != active_guest_turn.generation)
        return nullptr;
    return active_guest_turn.stack;
}
// Actor tickets must never be valid in two distinct carrier lifetimes.
// The saturated CAS prevents wraparound reuse (including after UINT64_MAX).
std::atomic<std::uint64_t> next_global_actor_generation{1};
std::uint64_t mint_actor_generation() {
    auto value = next_global_actor_generation.load(std::memory_order_relaxed);
    for (;;) {
        if (value == UINT64_MAX)
            throw std::overflow_error("actor generation namespace exhausted");
        if (next_global_actor_generation.compare_exchange_weak(
                value, value + 1, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return value;
    }
}
} // namespace

MultiActorCarrier::MultiActorCarrier(std::uint64_t fuel)
    : shared_(std::make_shared<Shared>()) {
    if (fuel == 0 || fuel > 512)
        throw std::invalid_argument("multi actor fuel must be within 1..512");
    shared_->fuel = fuel;
}

MultiActorCarrier::~MultiActorCarrier() noexcept {
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
        std::terminate(); // never free shared queues on their own worker
    request_stop();
    join();
}

MultiActorCarrier::ActorToken MultiActorCarrier::register_actor(
        void* state, Slice slice, Cancel cancel) {
    if (!state || !slice || !cancel)
        throw std::invalid_argument("actor state, slice and cancel required");
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->started || shared_->closing)
        throw std::logic_error("actor registration after carrier admission");
    if (shared_->registered == max_actors)
        throw std::overflow_error("multi actor registration capacity exhausted");
    // Never admit two actor identities to exactly the same root state.
    // This is a defense in depth check, NOT an alias/borrow proof for
    // arbitrary overlapping pointers or nested objects.
    for (std::size_t i = 0; i < shared_->registered; ++i)
        if (shared_->actors[i].state == state)
            throw std::invalid_argument("duplicate actor state root");
    // Mint identity before publishing the slot: a ticket from any earlier
    // carrier in this process cannot accidentally name this actor.
    const auto generation = mint_actor_generation();
    const std::size_t index = shared_->registered++;
    auto& actor = shared_->actors[index];
    actor.state = state;
    actor.slice = slice;
    actor.cancel = cancel;
    actor.generation = generation;
    if (!actor.guest_stack.initialize(generation))
        throw std::logic_error("actor stack could not be initialized");
    return {static_cast<std::uint32_t>(index + 1), actor.generation};
}

MultiActorCarrier::Producer MultiActorCarrier::producer() const noexcept {
    return Producer{std::weak_ptr<Shared>{shared_}};
}

bool MultiActorCarrier::guest_frame_push(ActorToken owner, std::size_t bytes,
                                         GuestFrameToken& out) noexcept {
    out = {};
    auto* stack = authenticated_stack(owner);
    return stack && stack->push(bytes, out);
}
bool MultiActorCarrier::guest_frame_read(ActorToken owner, GuestFrameToken frame,
                                         std::size_t offset, void* destination,
                                         std::size_t bytes) noexcept {
    auto* stack = authenticated_stack(owner);
    return stack && stack->read(frame, offset, destination, bytes);
}
bool MultiActorCarrier::guest_frame_write(ActorToken owner, GuestFrameToken frame,
                                          std::size_t offset, const void* source,
                                          std::size_t bytes) noexcept {
    auto* stack = authenticated_stack(owner);
    return stack && stack->write(frame, offset, source, bytes);
}
bool MultiActorCarrier::guest_frame_pop(ActorToken owner, GuestFrameToken frame) noexcept {
    auto* stack = authenticated_stack(owner);
    return stack && stack->pop(frame);
}
std::size_t MultiActorCarrier::guest_frame_depth(ActorToken owner) noexcept {
    auto* stack = authenticated_stack(owner);
    return stack ? stack->depth() : 0;
}

bool MultiActorCarrier::enqueue(Shared& state, std::size_t index) noexcept {
    auto& actor = state.actors[index];
    if (actor.queued) return true; // exactly one runnable ticket per actor
    if (state.ready_size >= max_actors) return false;
    state.ready[(state.ready_head + state.ready_size) % max_actors] = index;
    ++state.ready_size;
    actor.queued = true;
    state.wake.notify_one();
    return true;
}

bool MultiActorCarrier::Producer::try_send(
        ActorToken token, Message message) const noexcept {
    auto shared = state_.lock();
    if (!shared) return false;
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (!shared->started || shared->closing || token.slot == 0 ||
            token.slot > shared->registered)
        return false;
    auto& actor = shared->actors[token.slot - 1];
    if (actor.generation != token.generation || actor.cancel_requested ||
            actor.retired || actor.canceling || actor.size == mailbox_capacity)
        return false;
    // Reserve a runnable position before publishing the first envelope.
    if (!actor.active && !actor.queued &&
            !enqueue(*shared, token.slot - 1))
        return false;
    actor.mailbox[(actor.head + actor.size) % mailbox_capacity] = message;
    ++actor.size;
    ++actor.accepted;
    ++shared->counters.accepted;
    shared->wake.notify_one();
    return true;
}

bool MultiActorCarrier::Producer::request_cancel(ActorToken token) const noexcept {
    auto shared = state_.lock();
    if (!shared) return false;
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (!shared->started || shared->closing || token.slot == 0 ||
            token.slot > shared->registered)
        return false;
    auto& actor = shared->actors[token.slot - 1];
    if (actor.generation != token.generation || actor.cancel_requested ||
            actor.retired || actor.canceling)
        return false;
    actor.cancel_requested = true; // independent CONTROL, no mailbox capacity
    shared->wake.notify_one();
    return true;
}

void MultiActorCarrier::start() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->started || shared_->closing)
            throw std::logic_error("multi actor carrier starts once");
        shared_->started = true;
    }
    try {
        auto s = shared_;
        worker_ = std::thread([s] { worker_loop(s); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->closing = true;
        shared_->stopped = true;
        throw;
    }
}

void MultiActorCarrier::request_stop() noexcept {
    auto s = shared_;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->closing = true;
    if (!s->started) {
        for (std::size_t i = 0; i < s->registered; ++i)
            s->actors[i].guest_stack.retire();
        s->stopped = true;
    }
    s->wake.notify_one();
}

void MultiActorCarrier::join() noexcept {
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
        std::terminate();
    request_stop();
    if (worker_.joinable()) worker_.join();
    // External frame ownership may be retired after this point only.
}

MultiActorCarrier::Stats MultiActorCarrier::stats() const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    Stats stats = shared_->counters;
    if (shared_->stopped)
        stats.abandoned = stats.accepted - stats.completed;
    return stats;
}

MultiActorCarrier::ActorStats MultiActorCarrier::actor_stats(ActorToken token) const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (token.slot == 0 || token.slot > shared_->registered) return {};
    const auto& actor = shared_->actors[token.slot - 1];
    if (actor.generation != token.generation) return {};
    return {actor.generation, actor.accepted, actor.completed,
            actor.cooperations, actor.retired, actor.active};
}

bool MultiActorCarrier::stopped() const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->stopped;
}

void MultiActorCarrier::worker_loop(const std::shared_ptr<Shared>& state) noexcept {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->counters.worker_thread_tag =
            static_cast<std::uint64_t>(std::hash<std::thread::id>{}(
                std::this_thread::get_id()));
    }
    for (;;) {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->wake.wait(lock, [&] {
            if (state->closing || state->ready_size != 0) return true;
            for (std::size_t i = 0; i < state->registered; ++i)
                if (state->actors[i].cancel_requested &&
                        !state->actors[i].retired) return true;
            return false;
        });

        // CONTROL wins at every physical dispatch boundary, including a
        // cancellation arriving while its victim is executing guest code.
        std::size_t cancel_index = max_actors;
        for (std::size_t i = 0; i < state->registered; ++i) {
            const auto& actor = state->actors[i];
            if (!actor.retired && (state->closing || actor.cancel_requested)) {
                cancel_index = i;
                break;
            }
        }
        if (cancel_index != max_actors) {
            auto& actor = state->actors[cancel_index];
            actor.cancel_requested = true;
            actor.canceling = true;
            const bool had_active = actor.active;
            const auto message = actor.current;
            auto* user_state = actor.state;
            auto* cleanup = actor.cancel;
            lock.unlock();
            // Teardown runs under this actor's authority, never a sibling's.
            {
                ScopedGuestTurn guest_turn(&actor.guest_stack,
                    static_cast<std::uint32_t>(cancel_index + 1), actor.generation);
                cleanup(user_state, message, had_active);
            }
            actor.guest_stack.retire();
            // Acknowledge only AFTER cleanup and physical frame scrubbing.
            lock.lock();
            actor.active = false;
            actor.queued = false;
            actor.size = 0;
            actor.retired = true;
            actor.canceling = false;
            if (had_active) ++state->counters.cancelled_active;
            continue;
        }
        if (state->closing) {
            state->stopped = true;
            state->wake.notify_all();
            return;
        }
        if (state->ready_size == 0) continue;
        const std::size_t index = state->ready[state->ready_head];
        state->ready_head = (state->ready_head + 1) % max_actors;
        --state->ready_size;
        if (index >= state->registered) {
            ++state->counters.protocol_failures;
            state->closing = true;
            continue;
        }
        auto& actor = state->actors[index];
        if (!actor.queued || actor.retired || actor.cancel_requested)
            continue; // canceled, stale queued ticket cannot execute
        actor.queued = false;
        Cause cause = Cause::cpu_resume;
        if (!actor.active) {
            if (!actor.size) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
                continue;
            }
            actor.current = actor.mailbox[actor.head];
            actor.head = (actor.head + 1) % mailbox_capacity;
            --actor.size;
            actor.active = true;
            cause = Cause::initial;
        }
        const auto message = actor.current;
        auto* user_state = actor.state;
        auto* callback = actor.slice;
        const auto fuel = state->fuel;
        lock.unlock();
        Outcome outcome;
        {
            ScopedGuestTurn guest_turn(&actor.guest_stack,
                static_cast<std::uint32_t>(index + 1), actor.generation);
            outcome = callback(user_state, message, cause, fuel);
        }
        lock.lock();
        ++state->counters.dispatches;
        // A completed guest slice has ALREADY committed its source effects.
        // Count this logical turn exactly once, even if cancellation/STOP
        // arrived while the native callback was running. Cancellation then
        // retires the actor (and discards later envelopes) with no active
        // frame; it cannot retroactively turn DONE into a canceled frame.
        if (outcome == Outcome::complete) {
            // Native guest frames cannot leak into the next message. A bogus
            // leftover frame is a contract failure even though this callback's
            // already-published source effects cannot be uncommitted.
            const bool leaked_frames = actor.guest_stack.depth() != 0;
            actor.guest_stack.clear_turn();
            if (leaked_frames) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
            }
            actor.active = false;
            ++actor.completed;
            ++state->counters.completed;
            if (state->closing || actor.cancel_requested)
                continue; // process reserved CONTROL before other user work
            if (actor.size && !enqueue(*state, index)) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
            }
        } else if (outcome == Outcome::cooperate) {
            // This continuation is still live. CONTROL wins over requeue,
            // and owner-thread cancellation must run exactly once.
            if (state->closing || actor.cancel_requested) continue;
            ++actor.cooperations;
            ++state->counters.cooperations;
            if (!enqueue(*state, index)) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
            }
        } else {
            // A bogus callback result is a protocol failure even if a
            // cancellation raced it. Never silently accept invalid IR state.
            ++state->counters.protocol_failures;
            actor.cancel_requested = true;
        }
    }
}


// Compiler-owned, synchronous i64 actor parameter frame ABI. These functions
// require the *actual* MultiActorCarrier worker lease; raw ActorRef, actor IDs,
// pointers, or guest-supplied frame serials are never accepted as authority.
// Intentionally not a suspended-frame/continuation API.
extern "C" void oreslang_actor_guest_frame_i64_enter(std::int64_t value) noexcept {
    auto& turn = active_guest_turn;
    if (!turn.stack || turn.generation == 0 ||
            turn.generated_scalar.serial != 0 || turn.stack->depth() != 0)
        std::abort();
    ActorGuestFrameStack::FrameToken frame{};
    if (!turn.stack->push(sizeof(value), frame) ||
            frame.actor_generation != turn.generation ||
            !turn.stack->write(frame, 0, &value, sizeof(value)))
        std::abort();
    turn.generated_scalar = frame;
}

extern "C" std::int64_t oreslang_actor_guest_frame_i64_read() noexcept {
    const auto& turn = active_guest_turn;
    std::int64_t value = 0;
    if (!turn.stack || turn.generated_scalar.serial == 0 ||
            turn.generated_scalar.actor_generation != turn.generation ||
            !turn.stack->read(turn.generated_scalar, 0, &value, sizeof(value)))
        std::abort();
    return value;
}

extern "C" void oreslang_actor_guest_frame_i64_leave() noexcept {
    auto& turn = active_guest_turn;
    if (!turn.stack || turn.generated_scalar.serial == 0 ||
            turn.generated_scalar.actor_generation != turn.generation ||
            !turn.stack->pop(turn.generated_scalar))
        std::abort();
    turn.generated_scalar = {};
}

} // namespace oreslang::runtime
