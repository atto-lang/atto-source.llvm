#include "oreslang/runtime/HungryCarrier.hpp"

#include <functional>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace oreslang::runtime {

HungryCarrier::HungryCarrier(void* actor_state, ActorHandler handler,
                             void* broker_state, BrokerOperation operation,
                             Options options)
    : actor_state_(actor_state), handler_(handler), broker_state_(broker_state),
      operation_(operation), options_(options) {
    if (!handler_ || !operation_)
        throw std::invalid_argument("actor and I/O function pointers required");
    if (options_.require_exclusive_core || options_.require_strict_no_gc ||
        options_.require_hard_realtime)
        throw std::invalid_argument("strict CPU/GC/RT profile is not certified or implemented");
    // The bounded queue's algorithm relies on lock-free atomic indexes on the
    // supported host. Reject a platform that silently implements them with locks.
    if (!std::atomic<std::size_t>{}.is_lock_free())
        throw std::invalid_argument("lock-free atomic indexes unavailable");
    if (options_.spin_before_park.count() < 0 || options_.spin_before_park.count() > 100000)
        throw std::invalid_argument("spin window exceeds 100 ms maximum");
}

HungryCarrier::~HungryCarrier() noexcept {
    request_stop();
    join();
}

void HungryCarrier::start() {
    if (started_.exchange(true, std::memory_order_acq_rel))
        throw std::logic_error("carrier can only start once");
    try {
        broker_thread_ = std::thread([this] { broker_loop(); });
        actor_thread_ = std::thread([this] { actor_loop(); });
    } catch (...) {
        request_stop();
        join();
        throw;
    }
    // Startup is outside strict realtime hot path; verify CPU admission
    // before allowing any external message.
    while (!setup_done_.load(std::memory_order_acquire))
        std::this_thread::yield();
    if (!setup_ok_.load(std::memory_order_acquire)) {
        request_stop();
        join();
        throw std::runtime_error("actor CPU affinity admission failed");
    }
}

bool HungryCarrier::try_send(ActorMessage message) {
    if (!setup_ok_.load(std::memory_order_acquire) ||
        stop_.load(std::memory_order_acquire)) return false;
    if (!messages_.try_push(message)) return false;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    // The readiness predicate is published before the notification.
    queued_.fetch_add(1, std::memory_order_release);
    actor_signal_.notify();
    return true;
}

void HungryCarrier::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    auto expected = OperationTerminal::pending;
    if (terminal_.compare_exchange_strong(expected, OperationTerminal::canceled,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        canceled_.fetch_add(1, std::memory_order_relaxed);
    // Reserved control channel: independent of bounded user mailboxes.
    try { actor_signal_.notify(); } catch (...) { failures_.fetch_add(1, std::memory_order_relaxed); }
    try { broker_signal_.notify(); } catch (...) { failures_.fetch_add(1, std::memory_order_relaxed); }
}

void HungryCarrier::join() noexcept {
    // Calling join() from one of these two worker threads would self-deadlock.
    if (actor_thread_.joinable() && actor_thread_.get_id() != std::this_thread::get_id())
        actor_thread_.join();
    if (broker_thread_.joinable() && broker_thread_.get_id() != std::this_thread::get_id())
        broker_thread_.join();
}

CarrierStats HungryCarrier::stats() const noexcept {
    return {accepted_.load(std::memory_order_acquire),
            completed_.load(std::memory_order_acquire),
            io_done_.load(std::memory_order_acquire),
            parked_.load(std::memory_order_acquire),
            canceled_.load(std::memory_order_acquire),
            stale_.load(std::memory_order_acquire),
            failures_.load(std::memory_order_acquire),
            thread_tag_.load(std::memory_order_acquire)};
}

bool HungryCarrier::pin_cpu() noexcept {
    if (options_.pin_to_logical_cpu < 0) return true;
#if defined(__linux__)
    if (options_.pin_to_logical_cpu >= CPU_SETSIZE) return false;
    cpu_set_t chosen{};
    CPU_ZERO(&chosen);
    CPU_SET(options_.pin_to_logical_cpu, &chosen);
    if (pthread_setaffinity_np(pthread_self(), sizeof(chosen), &chosen) != 0) return false;
    cpu_set_t effective{};
    CPU_ZERO(&effective);
    if (pthread_getaffinity_np(pthread_self(), sizeof(effective), &effective) != 0) return false;
    return CPU_COUNT(&effective) == 1 &&
           CPU_ISSET(options_.pin_to_logical_cpu, &effective);
#else
    return false;
#endif
}

void HungryCarrier::run_handler(ActorMessage message, bool resumed, std::int64_t result) noexcept {
    if (stop_.load(std::memory_order_acquire)) return;
    const auto step = handler_(actor_state_, message, resumed, result);
    if (stop_.load(std::memory_order_acquire)) return;
    if (step.kind == StepKind::done) {
        completed_.fetch_add(1, std::memory_order_release);
        return;
    }
    if (step.kind != StepKind::await_io || pending_ ||
        generation_ == std::numeric_limits<std::uint64_t>::max()) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
        return;
    }
    ++generation_;
    // Only one logical operation is inflight with serial non-reentrancy.
    // Queue memory is preallocated; no owned actor pointer crosses threads.
    pending_ = true;
    current_ = message;
    terminal_.store(OperationTerminal::pending, std::memory_order_release);
    if (!jobs_.try_push(Job{generation_, step.value, step.delay_ms})) {
        pending_ = false;
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop(); // no silently lost I/O
        return;
    }
    jobs_available_.fetch_add(1, std::memory_order_release);
    try { broker_signal_.notify(); }
    catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
}

void HungryCarrier::wait_actor() noexcept {
    if (stop_.load(std::memory_order_acquire)) return;
    const auto ready = [this] {
        return (queued_.load(std::memory_order_acquire) != 0 && !pending_)
               || results_available_.load(std::memory_order_acquire) != 0
               || stop_.load(std::memory_order_acquire);
    };
    if (ready()) return;
    const auto spin_end = std::chrono::steady_clock::now() + options_.spin_before_park;
    while (std::chrono::steady_clock::now() < spin_end && !ready()) {
        // Supervisor cancellation is part of ready(), even on busy-poll.
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    if (ready()) return;
    parked_.fetch_add(1, std::memory_order_relaxed);
    try {
        (void)actor_signal_.wait_until(ready, stop_,
                                      std::chrono::milliseconds::max());
    } catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
}

void HungryCarrier::actor_loop() noexcept {
    const auto tag = std::hash<std::thread::id>{}(std::this_thread::get_id());
    thread_tag_.store(static_cast<std::uint64_t>(tag), std::memory_order_release);
    const bool admitted = pin_cpu();
    setup_ok_.store(admitted, std::memory_order_release);
    setup_done_.store(true, std::memory_order_release);
    if (!admitted) {
        request_stop();
        stopped_.store(true, std::memory_order_release);
        return;
    }
    while (!stop_.load(std::memory_order_acquire)) {
        if (pending_) {
            Completion c;
            if (results_available_.load(std::memory_order_acquire) != 0 &&
                results_.try_pop(c)) {
                results_available_.fetch_sub(1, std::memory_order_acq_rel);
                if (c.generation != generation_ ||
                    terminal_.load(std::memory_order_acquire) != OperationTerminal::completed) {
                    stale_.fetch_add(1, std::memory_order_relaxed);
                    continue; // never resume on stale/canceled operation
                }
                pending_ = false;
                io_done_.fetch_add(1, std::memory_order_release);
                run_handler(current_, true, c.value);
                continue;
            }
        } else if (queued_.load(std::memory_order_acquire) != 0) {
            ActorMessage m;
            if (messages_.try_pop(m)) {
                queued_.fetch_sub(1, std::memory_order_acq_rel);
                run_handler(m, false, 0);
                continue;
            }
        }
        wait_actor();
    }
    if (pending_) {
        auto expected = OperationTerminal::pending;
        if (terminal_.compare_exchange_strong(expected, OperationTerminal::canceled,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
            canceled_.fetch_add(1, std::memory_order_relaxed);
    }
    stopped_.store(true, std::memory_order_release);
}

void HungryCarrier::broker_loop() noexcept {
    while (!stop_.load(std::memory_order_acquire)) {
        Job job;
        if (jobs_available_.load(std::memory_order_acquire) &&
            jobs_.try_pop(job)) {
            jobs_available_.fetch_sub(1, std::memory_order_acq_rel);
            // A cancellable *mock latency* transport. A real syscall/uring
            // adapter must preserve buffer lifetime and acknowledge cancel.
            try {
                if (job.delay_ms) {
                    (void)broker_signal_.wait_until(
                        [] { return false; }, stop_,
                        std::chrono::milliseconds(job.delay_ms));
                }
                if (stop_.load(std::memory_order_acquire)) continue;
                const auto result = operation_(broker_state_, job.payload);
                if (stop_.load(std::memory_order_acquire)) continue;
                auto expected = OperationTerminal::pending;
                if (!terminal_.compare_exchange_strong(expected, OperationTerminal::completed,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
                    continue;  // canceled or obsolete; never produce second terminal event
                if (!results_.try_push(Completion{job.generation, result})) {
                    failures_.fetch_add(1, std::memory_order_relaxed);
                    request_stop(); // reserved 1-inflight completion may never drop
                    continue;
                }
                results_available_.fetch_add(1, std::memory_order_release);
                actor_signal_.notify();
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                request_stop();
            }
            continue;
        }
        try {
            (void)broker_signal_.wait_until(
                [this] { return jobs_available_.load(std::memory_order_acquire) != 0; },
                stop_, std::chrono::milliseconds::max());
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            request_stop();
        }
    }
}
} // namespace oreslang::runtime
