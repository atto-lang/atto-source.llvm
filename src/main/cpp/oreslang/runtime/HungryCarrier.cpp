#include "oreslang/runtime/HungryCarrier.hpp"

#include <functional>
#include <exception>
#include <thread>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace oreslang::runtime {

// The control block outlives the carrier whenever producer handles remain.
// An entrant increments active before checking closed. Closing first sets
// closed=true, then waits for every entrant with closed=false to exit.
struct HungryCarrier::ProducerGate {
    explicit ProducerGate(HungryCarrier* ptr) : carrier(ptr) {}
    HungryCarrier* const carrier;
    std::atomic<bool> closed{false};
    std::atomic<std::uint64_t> active{0};
};

bool HungryCarrier::ProducerHandle::try_send(ActorMessage message) const noexcept {
    auto gate = gate_; // local shared_ptr keeps control block alive
    if (!gate) return false;
    gate->active.fetch_add(1, std::memory_order_acq_rel);
    struct Release {
        std::atomic<std::uint64_t>& active;
        ~Release() { active.fetch_sub(1, std::memory_order_release); }
    } release{gate->active};
    if (gate->closed.load(std::memory_order_acquire)) return false;
    // The lifecycle owner cannot reclaim this pointer until all active
    // handles that observed open have left. A closed handle never loads it.
    try {
        return gate->carrier->try_send(message);
    } catch (...) {
        // The lower-level sender catches errors after a successful enqueue;
        // do not falsely claim an accepted message was never submitted.
        std::terminate();
    }
}

HungryCarrier::ProducerHandle HungryCarrier::producer_handle() const noexcept {
    return ProducerHandle{producer_gate_};
}

HungryCarrier::HungryCarrier(void* actor_state, ActorHandler handler,
                             void* broker_state, BrokerOperation operation,
                             Options options)
    : actor_state_(actor_state), handler_(handler), broker_state_(broker_state),
      operation_(operation), options_(options),
      producer_gate_(std::make_shared<ProducerGate>(this)) {
    if (!handler_ || !operation_)
        throw std::invalid_argument("actor and I/O function pointers required");
    if (options_.require_exclusive_core || options_.require_strict_no_gc ||
        options_.require_hard_realtime)
        throw std::invalid_argument("strict CPU/GC/RT profile is not certified or implemented");
    // The bounded queue's algorithm relies on lock-free atomic indexes on the
    // supported host. Reject a platform that silently implements them with locks.
    if (!std::atomic<std::size_t>::is_always_lock_free)
        throw std::invalid_argument("lock-free atomic indexes unavailable");
    if (options_.spin_before_park.count() < 0 || options_.spin_before_park.count() > 100000)
        throw std::invalid_argument("spin window exceeds 100 ms maximum");
}

HungryCarrier::~HungryCarrier() noexcept {
    // A self-destructing worker would close eventfds and free queues while
    // the thread is still accessing them. Fail closed rather than UAF.
    if ((actor_thread_.joinable() && actor_thread_.get_id() == std::this_thread::get_id()) ||
        (broker_thread_.joinable() && broker_thread_.get_id() == std::this_thread::get_id()))
        std::terminate();
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
    // The counter protects *already-entered* producers during stop/join.
    // It cannot make destruction concurrent with NEW method calls safe:
    // the external owner must quiesce initiation of new calls first.
    in_flight_senders_.fetch_add(1, std::memory_order_acq_rel);
    struct SenderExit {
        std::atomic<std::uint32_t>& count;
        ~SenderExit() { count.fetch_sub(1, std::memory_order_release); }
    } guard{in_flight_senders_};

    if (!setup_ok_.load(std::memory_order_acquire) ||
        stop_.load(std::memory_order_acquire)) return false;
    if (!messages_.try_push(message)) return false;
    // A stop racing with successful enqueue can abandon this accepted turn.
    // Report that explicitly through abandoned_on_stop after join.
    accepted_.fetch_add(1, std::memory_order_relaxed);
    try {
        actor_signal_.notify();
    } catch (...) {
        // The enqueue already succeeded. Returning false would invite a
        // duplicate retry of an accepted message.
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
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
    // join() is an explicit terminal lifecycle operation, not an indefinite
    // wait for a spontaneous stop. Only one thread may call join()/destroy.
    if ((actor_thread_.joinable() && actor_thread_.get_id() == std::this_thread::get_id()) ||
        (broker_thread_.joinable() && broker_thread_.get_id() == std::this_thread::get_id()))
        std::terminate();
    // Seal new handle admissions before beginning shutdown, then quiesce
    // all already-entered handle calls before freeing the carrier's storage.
    producer_gate_->closed.store(true, std::memory_order_release);
    request_stop();
    if (actor_thread_.joinable()) actor_thread_.join();
    if (broker_thread_.joinable()) broker_thread_.join();
    // Producers that entered before stop may still be publishing a payload
    // or notifying. Wait for them before the eventfds/queue can be destroyed.
    while (in_flight_senders_.load(std::memory_order_acquire) != 0 ||
           producer_gate_->active.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    const auto accepted = accepted_.load(std::memory_order_acquire);
    const auto completed = completed_.load(std::memory_order_acquire);
    if (completed > accepted) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        abandoned_on_stop_.store(0, std::memory_order_release);
    } else {
        abandoned_on_stop_.store(accepted - completed, std::memory_order_release);
    }
}

CarrierStats HungryCarrier::stats() const noexcept {
    return {accepted_.load(std::memory_order_acquire),
            completed_.load(std::memory_order_acquire),
            io_done_.load(std::memory_order_acquire),
            parked_.load(std::memory_order_acquire),
            canceled_.load(std::memory_order_acquire),
            stale_.load(std::memory_order_acquire),
            failures_.load(std::memory_order_acquire),
            thread_tag_.load(std::memory_order_acquire),
            abandoned_on_stop_.load(std::memory_order_acquire)};
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
    try { broker_signal_.notify(); }
    catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
}

void HungryCarrier::wait_actor() noexcept {
    if (stop_.load(std::memory_order_acquire)) return;
    const auto ready = [this] {
        return (!pending_ && messages_.has_ready())
               || (pending_ && results_.has_ready())
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
            if (results_.try_pop(c)) {
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
        } else {
            ActorMessage m;
            if (messages_.try_pop(m)) {
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
        if (jobs_.try_pop(job)) {
            // A cancellable *mock latency* transport. A real syscall/uring
            // adapter must preserve buffer lifetime and acknowledge cancel.
            try {
                if (job.delay_ms) {
                    (void)broker_signal_.wait_until(
                        [] { return false; }, stop_,
                        std::chrono::milliseconds(job.delay_ms));
                }
                if (stop_.load(std::memory_order_acquire)) continue;
                const auto result = operation_(broker_state_, job.payload, stop_);
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
                actor_signal_.notify();
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                request_stop();
            }
            continue;
        }
        try {
            (void)broker_signal_.wait_until(
                [this] { return jobs_.has_ready(); },
                stop_, std::chrono::milliseconds::max());
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            request_stop();
        }
    }
}
} // namespace oreslang::runtime
