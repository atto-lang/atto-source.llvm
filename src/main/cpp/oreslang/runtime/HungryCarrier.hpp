#pragma once
#include "oreslang/runtime/BoundedMpscQueue.hpp"
#include "oreslang/runtime/ParkingSignal.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// Experimental native continuation carrier. This is an independent C++
// runtime skeleton: not yet wired into parsed/compiled .ores actors, the
// ownership checker, IOCP, io_uring, or realtime CPU partition admission.
// No C++ closures/heap objects cross the actor mailbox: only POD messages.
namespace oreslang::runtime {

struct ActorMessage {
    std::uint64_t id = 0;
    std::int64_t input = 0;
};
enum class StepKind : std::uint8_t { done, await_io };
// Per-operation terminal state. Only a Pending->Completed or
// Pending->Canceled transition may win. A new operation resets the state
// only after the previous broker result has been consumed on this carrier.
enum class OperationTerminal : std::uint8_t { idle, pending, completed, canceled };
struct ActorStep {
    StepKind kind = StepKind::done;
    std::int64_t value = 0;        // completion value or payload for IO
    std::uint32_t delay_ms = 0;    // test transport delay (not production I/O)
};
// Invoked ONLY on the one actor carrier thread. The actor-local state pointer
// cannot be accessed by the IO worker. This callback may NOT block, throw,
// allocate or run arbitrary GC in a future strict-no-GC admission profile.
// 'resumed' is true exactly once when the pending IO has completed.
using ActorHandler = ActorStep (*)(void* actor_state, ActorMessage message,
                                   bool resumed, std::int64_t result) noexcept;
// The IO callback runs on a separate broker thread, with separate owned IO
// state: it must NOT dereference the actor's private heap or use the carrier.
// It must periodically check the cancellation flag and use bounded/nonblocking
// syscalls. Cooperative cancellation does not forcibly interrupt arbitrary FFI.
using BrokerOperation = std::int64_t (*)(void* broker_state, std::int64_t payload,
                                         const std::atomic<bool>& cancel) noexcept;

struct CarrierStats {
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::uint64_t io_completions = 0;
    std::uint64_t parked = 0;
    std::uint64_t canceled_inflight = 0;
    std::uint64_t stale_completions = 0;
    std::uint64_t protocol_failures = 0;
    std::uint64_t actor_thread_tag = 0;
    std::uint64_t abandoned_on_stop = 0; // accepted minus finished turns; valid after join()
};

// Construction and thread start allocate and use syscalls, BEFORE any future
// realtime admission. Hot-path buffers are fixed-size. This deliberately does
// not claim that callbacks, syscalls or OS scheduling are allocation-free.
class HungryCarrier final {
public:
    static constexpr std::size_t queue_capacity = 64;
    struct Options {
        std::chrono::microseconds spin_before_park{0}; // park-only default
        int pin_to_logical_cpu = -1; // optional affinity; NOT exclusive-core admission
        bool require_exclusive_core = false;  // fail closed until OS reservation manager exists
        bool require_strict_no_gc = false;    // fail closed until compiler/allocator proof exists
        bool require_hard_realtime = false;   // fail closed until watchdog/OS proof exists
    };
    HungryCarrier(void* actor_state, ActorHandler handler, void* broker_state,
                  BrokerOperation operation, Options opts);
    ~HungryCarrier() noexcept;
    HungryCarrier(const HungryCarrier&) = delete;
    HungryCarrier& operator=(const HungryCarrier&) = delete;

    void start();
    // True means ownership of the request transferred to the bounded queue,
    // NOT guaranteed execution after concurrent stop(). False means queue
    // pressure, no admission, or stop. Callers must cease issuing new calls
    // before destroying the carrier; join quiesces already-active send calls.
    // There must be one lifecycle owner calling join()/destructor.
    bool try_send(ActorMessage message);
    // Stop is an independent reserved control path (not an ordinary mailbox).
    void request_stop() noexcept;
    // A single external lifecycle owner calls join; it also initiates stop.
    // Joining/destroying from the actor or broker thread is fatal, not safe.
    void join() noexcept;
    bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }
    CarrierStats stats() const noexcept;

private:
    struct Job {
        std::uint64_t generation = 0;
        std::int64_t payload = 0;
        std::uint32_t delay_ms = 0;
    };
    struct Completion {
        std::uint64_t generation = 0;
        std::int64_t value = 0;
    };
    void actor_loop() noexcept;
    void broker_loop() noexcept;
    void run_handler(ActorMessage, bool resumed, std::int64_t result) noexcept;
    void wait_actor() noexcept;
    bool pin_cpu() noexcept;

    void* actor_state_;
    ActorHandler handler_;
    void* broker_state_;
    BrokerOperation operation_;
    Options options_;
    BoundedMpscQueue<ActorMessage, queue_capacity> messages_;
    BoundedMpscQueue<Job, 2> jobs_;          // one producer: carrier
    BoundedMpscQueue<Completion, 2> results_; // one producer: broker
    ParkingSignal actor_signal_;
    ParkingSignal broker_signal_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::uint32_t> in_flight_senders_{0};
    std::atomic<bool> setup_done_{false};
    std::atomic<bool> setup_ok_{false};
    std::atomic<OperationTerminal> terminal_{OperationTerminal::idle};
    std::thread actor_thread_;
    std::thread broker_thread_;
    // Owned/exclusively accessed by carrier:
    bool pending_ = false;
    ActorMessage current_{};
    std::uint64_t generation_ = 0;
    // Atomic telemetry allows reads without touching actor-local state.
    std::atomic<std::uint64_t> accepted_{0}, completed_{0}, io_done_{0},
        parked_{0}, canceled_{0}, stale_{0}, failures_{0}, thread_tag_{0},
        abandoned_on_stop_{0};
};
} // namespace oreslang::runtime
