#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

// Experimental *multi-actor* CPU carrier. Unlike HungryCarrier, one OS worker
// serially dispatches up to 8 distinct actor contexts in round-robin order.
// No IO, borrowed frame, GC, or realtime admission is implemented here.
namespace oreslang::runtime {

class MultiActorCarrier final {
    struct Shared;
public:
    static constexpr std::size_t max_actors = 8;
    static constexpr std::size_t mailbox_capacity = 64;
    static constexpr std::uint64_t default_fuel = 32;

    struct ActorToken {
        std::uint32_t slot = 0; // slots are 1-based; zero is never valid
        std::uint64_t generation = 0;
    };
    struct Message {
        std::uint64_t sequence = 0;
        std::int64_t payload = 0;
    };
    enum class Cause : std::uint8_t { initial, cpu_resume };
    enum class Outcome : std::uint8_t { complete, cooperate };
    // Called only on this carrier's worker, once per bounded dispatch slice.
    // A live in-flight turn is never reentrant and must retain its own frame.
    using Slice = Outcome (*)(void*, Message, Cause, std::uint64_t fuel) noexcept;
    // Invoked on the worker after the running slice has returned, never from
    // the producer. Host frame ownership persists until cancel ACK and join.
    using Cancel = void (*)(void*, Message active, bool had_active) noexcept;

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t cooperations = 0;
        std::uint64_t dispatches = 0;
        std::uint64_t cancelled_active = 0;
        std::uint64_t abandoned = 0;
        std::uint64_t protocol_failures = 0;
        std::uint64_t worker_thread_tag = 0;
    };
    struct ActorStats {
        std::uint64_t generation = 0;
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t cooperations = 0;
        bool retired = false;
        bool active = false;
    };

    // A weak lifetime handle never dereferences an already freed carrier.
    // Locking the shared state serializes acceptance vs close / cancellation.
    class Producer final {
    public:
        Producer() = default;
        bool try_send(ActorToken actor, Message message) const noexcept;
        bool request_cancel(ActorToken actor) const noexcept;
    private:
        friend class MultiActorCarrier;
        explicit Producer(std::weak_ptr<Shared> state) : state_(std::move(state)) {}
        std::weak_ptr<Shared> state_;
    };

    explicit MultiActorCarrier(std::uint64_t fuel = default_fuel);
    ~MultiActorCarrier() noexcept;
    MultiActorCarrier(const MultiActorCarrier&) = delete;
    MultiActorCarrier& operator=(const MultiActorCarrier&) = delete;

    // Registration is setup-only. Each actor gets a unique token and its
    // caller-owned state must outlive join(). Slots are never reused.
    ActorToken register_actor(void* state, Slice slice, Cancel cancel);
    Producer producer() const noexcept;
    void start();
    void request_stop() noexcept; // reserved CONTROL; independent of user mail
    void join() noexcept;         // external lifecycle owner, only one joiner
    Stats stats() const noexcept;
    ActorStats actor_stats(ActorToken token) const noexcept;
    bool stopped() const noexcept;
private:
    struct Actor {
        void* state = nullptr;
        Slice slice = nullptr;
        Cancel cancel = nullptr;
        std::uint64_t generation = 0;
        std::array<Message, mailbox_capacity> mailbox{};
        std::size_t head = 0, size = 0;
        bool active = false, queued = false, cancel_requested = false;
        bool canceling = false, retired = false;
        Message current{};
        std::uint64_t accepted = 0, completed = 0, cooperations = 0;
    };
    struct Shared {
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::array<Actor, max_actors> actors{};
        std::array<std::size_t, max_actors> ready{};
        std::size_t ready_head = 0, ready_size = 0, registered = 0;
        std::uint64_t fuel = default_fuel;
        bool started = false, closing = false, stopped = false;
        Stats counters{};
    };

    static bool enqueue(Shared& state, std::size_t index) noexcept;
    static void worker_loop(const std::shared_ptr<Shared>& state) noexcept;
    std::shared_ptr<Shared> shared_;
    std::thread worker_;
};

} // namespace oreslang::runtime
