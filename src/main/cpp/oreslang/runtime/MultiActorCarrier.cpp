#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <functional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

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
    if (shared_->registered == max_actors ||
            shared_->next_generation == UINT64_MAX)
        throw std::overflow_error("multi actor registration capacity exhausted");
    const std::size_t index = shared_->registered++;
    auto& actor = shared_->actors[index];
    actor.state = state;
    actor.slice = slice;
    actor.cancel = cancel;
    actor.generation = shared_->next_generation++;
    return {static_cast<std::uint32_t>(index + 1), actor.generation};
}

MultiActorCarrier::Producer MultiActorCarrier::producer() const noexcept {
    return Producer{std::weak_ptr<Shared>{shared_}};
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
    if (!s->started) s->stopped = true;
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
            // Acknowledge only AFTER this owner-thread cleanup returns.
            cleanup(user_state, message, had_active);
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
        const auto outcome = callback(user_state, message, cause, fuel);
        lock.lock();
        ++state->counters.dispatches;
        // A cancellation or stop that raced a bounded CPU step wins BEFORE
        // the continuation is requeued, and before any other user callback.
        if (state->closing || actor.cancel_requested) continue;
        if (outcome == Outcome::complete) {
            actor.active = false;
            ++actor.completed;
            ++state->counters.completed;
            if (actor.size && !enqueue(*state, index)) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
            }
        } else if (outcome == Outcome::cooperate) {
            ++actor.cooperations;
            ++state->counters.cooperations;
            if (!enqueue(*state, index)) {
                ++state->counters.protocol_failures;
                actor.cancel_requested = true;
            }
        } else {
            ++state->counters.protocol_failures;
            actor.cancel_requested = true;
        }
    }
}

} // namespace oreslang::runtime
