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

namespace {
thread_local HungryCarrier::NativeHeap* tls_actor_heap = nullptr;
// A self-join check based on std::thread::joinable()/get_id() can race a
// supervisor calling std::thread::join(). This TLS identity is set BEFORE
// carrier/broker callbacks and can be checked without touching thread objects
// or waiting behind the joiner's lifecycle mutex.
thread_local const HungryCarrier* tls_executing_carrier = nullptr;
struct CarrierExecutionScope final {
    explicit CarrierExecutionScope(const HungryCarrier* carrier) noexcept {
        if (tls_executing_carrier != nullptr) std::terminate();
        tls_executing_carrier = carrier;
    }
    CarrierExecutionScope(const CarrierExecutionScope&) = delete;
    CarrierExecutionScope& operator=(const CarrierExecutionScope&) = delete;
    ~CarrierExecutionScope() noexcept { tls_executing_carrier = nullptr; }
};
}

HungryCarrier::NativeHeap* HungryCarrier::active_heap() noexcept {
    return tls_actor_heap;
}

// An independent shared control block outlives the carrier. One atomic
// admission word linearizes close versus in-flight producer leases.
struct HungryCarrier::ProducerGate {
    explicit ProducerGate(HungryCarrier* ptr) : carrier(ptr) {}
    static constexpr std::uint64_t closing_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = closing_bit - 1;
    HungryCarrier* const carrier;
    // One atomic linearizes admission against shutdown. Separate closed and
    // active atomics permit a lifecycle observer to miss an entrant.
    std::atomic<std::uint64_t> leases{0};
};

bool HungryCarrier::ProducerHandle::try_send(ActorMessage message) const noexcept {
    auto gate = gate_; // local shared_ptr keeps control block alive
    if (!gate) return false;
    auto state = gate->leases.load(std::memory_order_acquire);
    for (;;) {
        if (state & ProducerGate::closing_bit) return false;
        if ((state & ProducerGate::count_mask) == ProducerGate::count_mask)
            return false; // bounded overflow; fail closed
        if (gate->leases.compare_exchange_weak(state, state + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
            break;
    }
    struct Release {
        std::atomic<std::uint64_t>& leases;
        ~Release() { leases.fetch_sub(1, std::memory_order_release); }
    } release{gate->leases};
    // If shutdown's fetch_or comes first, lease acquisition fails;
    // otherwise shutdown sees this held lease and waits before free.
    try {
        return gate->carrier->enqueue_mpsc(message);
    } catch (...) {
        std::terminate();
    }
}

bool HungryCarrier::ProducerHandle::try_wake_control() const noexcept {
    auto gate = gate_;
    if (!gate) return false;
    auto state = gate->leases.load(std::memory_order_acquire);
    for (;;) {
        if (state & ProducerGate::closing_bit) return false;
        if ((state & ProducerGate::count_mask) == ProducerGate::count_mask)
            return false;
        if (gate->leases.compare_exchange_weak(state, state + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
            break;
    }
    struct LeaseGuard {
        std::atomic<std::uint64_t>& leases;
        ~LeaseGuard() { leases.fetch_sub(1, std::memory_order_release); }
    } guard{gate->leases};
    return gate->carrier->wake_control();
}

HungryCarrier::ProducerHandle HungryCarrier::producer_handle() const noexcept {
    return ProducerHandle{producer_gate_};
}

bool HungryCarrier::DedicatedProducer::try_send(ActorMessage message) const noexcept {
    // Immediate rejection of concurrent use avoids converting a single-
    // producer lane into an unsafe MPSC queue. Move only between calls.
    if (sending_.test_and_set(std::memory_order_acquire)) return false;
    struct SendingGuard {
        std::atomic_flag& flag;
        ~SendingGuard() { flag.clear(std::memory_order_release); }
    } send_guard{sending_};

    auto gate = gate_;
    if (!gate) return false;
    auto state = gate->leases.load(std::memory_order_acquire);
    for (;;) {
        if (state & ProducerGate::closing_bit) return false;
        if ((state & ProducerGate::count_mask) == ProducerGate::count_mask)
            return false;
        if (gate->leases.compare_exchange_weak(state, state + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
            break;
    }
    struct LeaseGuard {
        std::atomic<std::uint64_t>& leases;
        ~LeaseGuard() { leases.fetch_sub(1, std::memory_order_release); }
    } lease_guard{gate->leases};
    return gate->carrier->try_send_spsc(index_, message);
}

HungryCarrier::DedicatedProducer HungryCarrier::register_spsc_producer() {
    // Lock in the same lifecycle -> lane-registration order as start().
    // Otherwise a join-before-start could certify retirement while a
    // caller still registers a new producer on the logically dead actor.
    std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    std::lock_guard<std::mutex> lock{lane_registration_mutex_};
    if (joined_.load(std::memory_order_acquire) ||
            stop_.load(std::memory_order_acquire) ||
            (producer_gate_->leases.load(std::memory_order_acquire) &
                ProducerGate::closing_bit))
        throw std::logic_error("SPSC producer registration after terminal stop");
    if (started_.load(std::memory_order_acquire))
        throw std::logic_error("SPSC producer registration is only allowed before start");
    if (registered_lanes_ >= max_dedicated_lanes)
        throw std::length_error("all bounded hungry producer lanes are reserved");
    return DedicatedProducer{producer_gate_, registered_lanes_++};
}

bool HungryCarrier::try_send_spsc(std::size_t lane, ActorMessage message) noexcept {
    if (lane >= registered_lanes_ ||
        !setup_ok_.load(std::memory_order_acquire) ||
        stop_.load(std::memory_order_acquire))
        return false;
    if (!lanes_[lane].try_push(message)) return false;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    try { actor_signal_.notify(); }
    catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
    return true;
}

bool HungryCarrier::has_input_ready() const noexcept {
    if (messages_.has_ready()) return true;
    for (std::size_t i = 0; i < registered_lanes_; ++i)
        if (lanes_[i].has_ready()) return true;
    return false;
}

bool HungryCarrier::pop_next_input(ActorMessage& message) noexcept {
    const std::size_t sources = registered_lanes_ + 1; // lane 0 = legacy MPSC
    for (std::size_t step = 0; step < sources; ++step) {
        const auto index = (next_input_ + step) % sources;
        const bool found = index == 0 ? messages_.try_pop(message)
                                      : lanes_[index - 1].try_pop(message);
        if (found) {
            next_input_ = (index + 1) % sources;
            return true;
        }
    }
    return false;
}

void HungryCarrier::bind_retirement_cohort() {
    if (options_.retirement_cohort != nullptr) {
        const auto owner = options_.retirement_cohort->owner();
        if (options_.heap_owner.has_value() && *options_.heap_owner != owner)
            throw std::invalid_argument("heap owner does not match retirement cohort");
        options_.heap_owner = owner;
    } else if (options_.heap_owner.has_value()) {
        // Backward-compatible single-lane path. The live-cohort registry still
        // rejects a second carrier for the same actor generation.
        options_.retirement_cohort =
            ActorHeapRetirementCohort::create(*options_.heap_owner, 1);
    }
    if (options_.retirement_cohort != nullptr) {
        if (!options_.retirement_cohort->register_lane())
            throw std::invalid_argument("actor retirement cohort lane capacity exceeded");
        retirement_lane_registered_ = true;
    }
}

HungryCarrier::HungryCarrier(void* actor_state, ActorHandler handler,
                             void* broker_state, BrokerOperation operation,
                             Options options)
    : actor_state_(actor_state), handler_(handler), broker_state_(broker_state),
      operation_(operation), options_(options),
      producer_gate_(std::make_shared<ProducerGate>(this)) {
    if (!handler_ || !operation_)
        throw std::invalid_argument("actor and I/O function pointers required");
    if (options_.cancel_handler != nullptr)
        throw std::invalid_argument("generated continuation cancellation hook requires V2 handler");
    if ((options_.supervisor_control == nullptr)
            != (options_.supervisor_hooks == nullptr))
        throw std::invalid_argument("supervisor CONTROL lane requires typed hooks");
    if (options_.require_exclusive_core || options_.require_strict_no_gc ||
        options_.require_hard_realtime)
        throw std::invalid_argument("strict CPU/GC/RT profile is not certified or implemented");
    // The bounded queue's algorithm relies on lock-free atomic indexes on the
    // supported host. Reject a platform that silently implements them with locks.
    if (!std::atomic<std::size_t>::is_always_lock_free)
        throw std::invalid_argument("lock-free atomic indexes unavailable");
    if (options_.spin_before_park.count() < 0 || options_.spin_before_park.count() > 100000)
        throw std::invalid_argument("spin window exceeds 100 ms maximum");
    bind_retirement_cohort();
}

HungryCarrier::HungryCarrier(void* actor_state, ActorHandlerV2 handler,
                             void* broker_state, BrokerOperation operation,
                             Options options)
    : actor_state_(actor_state), handler_v2_(handler), broker_state_(broker_state),
      operation_(operation), options_(options),
      producer_gate_(std::make_shared<ProducerGate>(this)) {
    if (!handler_v2_ || !operation_)
        throw std::invalid_argument("actor and I/O function pointers required");
    if ((options_.supervisor_control == nullptr) !=
        (options_.supervisor_hooks == nullptr))
        throw std::invalid_argument("supervisor CONTROL lane requires typed hooks");
    if (options_.require_exclusive_core || options_.require_strict_no_gc ||
        options_.require_hard_realtime)
        throw std::invalid_argument("strict CPU/GC/RT profile is not certified or implemented");
    if (!std::atomic<std::size_t>::is_always_lock_free)
        throw std::invalid_argument("lock-free atomic indexes unavailable");
    if (options_.spin_before_park.count() < 0 || options_.spin_before_park.count() > 100000)
        throw std::invalid_argument("spin window exceeds 100 ms maximum");
    bind_retirement_cohort();
}

HungryCarrier::~HungryCarrier() noexcept {
    // A worker may never destroy its own carrier. TLS avoids racing an
    // external std::thread::join() while inspecting the std::thread object.
    if (tls_executing_carrier == this) std::terminate();
    request_stop();
    join();
}

void HungryCarrier::start() {
    // A terminal join may happen before the first start, or race thread
    // creation. Serialize the entire setup publication with join(): a
    // retired actor generation must never spawn threads after a valid
    // retirement proof has been minted.
    std::unique_lock<std::mutex> lifecycle{lifecycle_mutex_};
    {
        std::lock_guard<std::mutex> lock{lane_registration_mutex_};
        if (joined_.load(std::memory_order_acquire) ||
                stop_.load(std::memory_order_acquire) ||
                (producer_gate_->leases.load(std::memory_order_acquire) &
                    ProducerGate::closing_bit))
            throw std::logic_error("stopped or joined carrier cannot start");
        if (started_.exchange(true, std::memory_order_acq_rel))
            throw std::logic_error("carrier can only start once");
    }
    try {
        broker_thread_ = std::thread([this] { broker_loop(); });
        actor_thread_ = std::thread([this] { actor_loop(); });
    } catch (...) {
        request_stop();
        lifecycle.unlock(); // join() serializes on the same lifecycle mutex.
        join();
        throw;
    }
    // Startup is outside strict realtime hot path; verify CPU admission
    // before allowing any external message. join() cannot certify physical
    // quiescence before setup has completed and both threads are published.
    while (!setup_done_.load(std::memory_order_acquire))
        std::this_thread::yield();
    if (!setup_ok_.load(std::memory_order_acquire)) {
        request_stop();
        lifecycle.unlock();
        join();
        throw std::runtime_error("actor CPU affinity admission failed");
    }
}

bool HungryCarrier::wake_control() noexcept {
    if (options_.supervisor_control == nullptr
            || !setup_ok_.load(std::memory_order_acquire)
            || stop_.load(std::memory_order_acquire)
            || !options_.supervisor_control->urgent()) return false;
    try {
        actor_signal_.notify();
    } catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
        return false;
    }
    return true;
}

bool HungryCarrier::enqueue_mpsc(ActorMessage message) noexcept {
    // Can only be reached while a ProducerHandle owns a pre-close gate lease.
    if (!setup_ok_.load(std::memory_order_acquire) ||
        stop_.load(std::memory_order_acquire)) return false;
    if (!messages_.try_push(message)) return false;
    // A stop racing with successful enqueue may abandon the accepted turn.
    accepted_.fetch_add(1, std::memory_order_relaxed);
    try { actor_signal_.notify(); }
    catch (...) {
        // Successful enqueue is still accepted even if the wake failed:
        // false would invite a dangerous duplicate retry.
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
    }
    return true;
}

void HungryCarrier::request_stop() noexcept {
    // The stop store and a new pending-I/O publication participate in one
    // sequentially consistent handshake. Either this request_stop() sees
    // the published pending terminal, or the carrier's post-publication
    // stop load sees this stop. Without the second check, STOP can land
    // between the handler's initial stop test and terminal_.store(pending).
    stop_.store(true, std::memory_order_seq_cst);
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
    // Detect actor/broker self-join BEFORE taking the lifecycle mutex.
    // Reading std::thread::joinable() here would itself race an external join;
    // TLS gives an independent, carrier-local execution identity.
    if (tls_executing_carrier == this) std::terminate();
    // join() is an explicit terminal lifecycle operation, not an indefinite
    // wait for a spontaneous stop. Concurrent start/join must not permit
    // startup after a cohort's terminal acknowledgment.
    std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    if ((actor_thread_.joinable() && actor_thread_.get_id() == std::this_thread::get_id()) ||
        (broker_thread_.joinable() && broker_thread_.get_id() == std::this_thread::get_id()))
        std::terminate();
    // Seal new handle admissions before beginning shutdown, then quiesce
    // all already-entered handle calls before freeing the carrier's storage.
    producer_gate_->leases.fetch_or(ProducerGate::closing_bit, std::memory_order_acq_rel);
    request_stop();
    if (actor_thread_.joinable()) actor_thread_.join();
    if (broker_thread_.joinable()) broker_thread_.join();
    // Both workers are quiescent now. An outstanding PENDING terminal means
    // STOP lost the registration race and no cancellation publication won.
    // Quarantine retirement rather than certify a possibly unacknowledged
    // broker operation or producer-owned resource.
    if (terminal_.load(std::memory_order_acquire) == OperationTerminal::pending) {
        retirement_cleanup_blocked_.store(true, std::memory_order_release);
        failures_.fetch_add(1, std::memory_order_relaxed);
    }
    // Once the admission word is closed, only previously leased producer
    // handles can still touch actor-owned queue and notification storage.
    while ((producer_gate_->leases.load(std::memory_order_acquire) &
            ProducerGate::count_mask) != 0)
        std::this_thread::yield();
    const auto accepted = accepted_.load(std::memory_order_acquire);
    const auto completed = completed_.load(std::memory_order_acquire);
    if (completed > accepted) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        abandoned_on_stop_.store(0, std::memory_order_release);
    } else {
        abandoned_on_stop_.store(accepted - completed, std::memory_order_release);
    }
    if (retirement_lane_registered_ && !retirement_lane_acknowledged_) {
        const bool cleanup_ok =
            !retirement_cleanup_blocked_.load(std::memory_order_acquire);
        options_.retirement_cohort->acknowledge_lane(cleanup_ok);
        retirement_lane_acknowledged_ = true;
    }
    // join-before-start also represents a physically stopped actor. The
    // worker's stopped_ publication cannot run when no thread was created.
    stopped_.store(true, std::memory_order_release);
    joined_.store(true, std::memory_order_release);
}

std::optional<CarrierRetirementProof> HungryCarrier::retirement_proof() const noexcept {
    if (!joined_.load(std::memory_order_acquire) ||
            options_.retirement_cohort == nullptr ||
            retirement_cleanup_blocked_.load(std::memory_order_acquire) ||
            !options_.retirement_cohort->ready_for_retirement())
        return std::nullopt;
    return CarrierRetirementProof{options_.retirement_cohort};
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
            abandoned_on_stop_.load(std::memory_order_acquire),
            cooperations_.load(std::memory_order_acquire),
            cancel_callbacks_.load(std::memory_order_acquire),
            cancel_callback_failures_.load(std::memory_order_acquire),
            unclean_abandonments_.load(std::memory_order_acquire)};
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

void HungryCarrier::run_handler(ActorMessage message, ResumeCause cause, std::int64_t result) noexcept {
    if (stop_.load(std::memory_order_acquire)) return;
    if (cause == ResumeCause::initial) {
        if (turn_active_) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            request_stop();
            return;
        }
        turn_active_ = true;
        current_ = message;
    } else if (!turn_active_) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
        return;
    }
    // Only actor handler execution may access this heap. Suspension does not
    // invalidate its handles; broker callbacks have no such capability.
    tls_actor_heap = &actor_heap_;
    const auto step = handler_v2_ != nullptr
        ? handler_v2_(actor_state_, message, cause, result)
        : handler_(actor_state_, message, cause == ResumeCause::io, result);
    tls_actor_heap = nullptr;

    // A concurrently requested stop does not erase a completion that already
    // happened inside this callback. DONE wins the logical-turn accounting;
    // nonterminal frames remain active for carrier-thread cancellation cleanup.
    if (step.kind == StepKind::done) {
        cooperating_ = false;
        pending_ = false;
        turn_active_ = false;
        completed_.fetch_add(1, std::memory_order_release);
        return;
    }
    if (stop_.load(std::memory_order_acquire)) return;

    if (step.kind == StepKind::cooperate) {
        // CPU handoff is not IO and not logical turn completion. Do not admit
        // subsequent user envelopes while this frame is runnable. A V1
        // callback MUST NOT get an invented 'resumed=true' CPU wakeup.
        if (!handler_v2_ || pending_ || step.value != 0 || step.delay_ms != 0) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            request_stop();
            return;
        }
        current_ = message;
        cooperating_ = true;
        cooperations_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (step.kind != StepKind::await_io || pending_ ||
        generation_ == std::numeric_limits<std::uint64_t>::max()) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        request_stop();
        return;
    }
    cooperating_ = false;
    ++generation_;
    // Only one logical operation is inflight with serial non-reentrancy.
    // Queue memory is preallocated; no owned actor pointer crosses threads.
    pending_ = true;
    current_ = message;
    terminal_.store(OperationTerminal::pending, std::memory_order_seq_cst);
    // CLOSE/REGISTER interlock: request_stop() first publishes stop=true,
    // then attempts pending->canceled. We publish pending first, then
    // recheck stop. No execution order permits a pending terminal to be
    // installed after STOP without one side also canceling it. Do not
    // enqueue a broker job after this stop has already been observed.
    if (stop_.load(std::memory_order_seq_cst)) {
        auto expected = OperationTerminal::pending;
        if (terminal_.compare_exchange_strong(expected, OperationTerminal::canceled,
                std::memory_order_acq_rel, std::memory_order_acquire))
            canceled_.fetch_add(1, std::memory_order_relaxed);
        return; // actor_loop runs exactly-once generated frame cleanup
    }
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

void HungryCarrier::cancel_active_turn() noexcept {
    if (!turn_active_) return;
    // Exactly one actor-carrier thread owns this transition. The broker cannot
    // see actor memory, and terminal_ has already canceled any pending I/O.
    if (options_.cancel_handler != nullptr) {
        tls_actor_heap = &actor_heap_;
        const bool cleaned = options_.cancel_handler(actor_state_, current_);
        tls_actor_heap = nullptr;
        cancel_callbacks_.fetch_add(1, std::memory_order_release);
        if (!cleaned) {
            cancel_callback_failures_.fetch_add(1, std::memory_order_relaxed);
            retirement_cleanup_blocked_.store(true, std::memory_order_release);
            failures_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // A started frame existed but no generated cancellation/drop authority
        // was supplied. It is safe to stop execution, but NOT to certify that
        // external actor-owned memory can be retired.
        unclean_abandonments_.fetch_add(1, std::memory_order_relaxed);
        retirement_cleanup_blocked_.store(true, std::memory_order_release);
    }
    pending_ = false;
    cooperating_ = false;
    turn_active_ = false;
}

void HungryCarrier::wait_actor() noexcept {
    if (stop_.load(std::memory_order_acquire)) return;
    const auto ready = [this] {
        return (options_.supervisor_control != nullptr
                    && options_.supervisor_control->urgent())
               || (cooperating_ || (!pending_ && has_input_ready()))
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
    const CarrierExecutionScope active{this};
    const auto tag = std::hash<std::thread::id>{}(std::this_thread::get_id());
    thread_tag_.store(static_cast<std::uint64_t>(tag), std::memory_order_release);
    const bool admitted = pin_cpu() && actor_heap_.bind_carrier();
    setup_ok_.store(admitted, std::memory_order_release);
    setup_done_.store(true, std::memory_order_release);
    if (!admitted) {
        request_stop();
        stopped_.store(true, std::memory_order_release);
        return;
    }
    while (!stop_.load(std::memory_order_acquire)) {
        // Parent CONTROL runs before user envelopes or IO continuations on
        // this actor's own carrier, never on the producer/failed child's
        // thread. The typed lane owns its independent bounded capacity.
        if (options_.supervisor_control != nullptr
                && options_.supervisor_control->urgent()) {
            try {
                (void)options_.supervisor_control->admit_turn(
                    *options_.supervisor_hooks, false);
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                request_stop();
            }
            continue;
        }
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
                run_handler(current_, ResumeCause::io, c.value);
                continue;
            }
        } else {
            // CPU continuation precedes another envelope for this same actor.
            // Parent CONTROL is dispatched at the top of every physical turn.
            if (cooperating_) {
                run_handler(current_, ResumeCause::cpu, 0);
                continue;
            }
            ActorMessage m;
            if (pop_next_input(m)) {
                run_handler(m, ResumeCause::initial, 0);
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
    cancel_active_turn();
    stopped_.store(true, std::memory_order_release);
}

void HungryCarrier::broker_loop() noexcept {
    const CarrierExecutionScope active{this};
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
