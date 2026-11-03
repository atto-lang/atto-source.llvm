#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace oreslang::runtime {
// Opt-in host substrate. Does not grant actor/guest execution authority.
// Reactor owns registrations and retains completions until try_submit accepts.
// poll must return within 10ms; wake must be sticky across poll entry, nonblocking,
// thread-safe and noexcept. poll may enqueue host records, never invoke guest code.
class SharedReactorPool final {
public:
    struct Reactor {
        virtual ~Reactor() = default;
        virtual void poll(bool may_wait) noexcept = 0;
        virtual void wake() noexcept = 0;
    };
    using Work = void (*)(void*);
    static constexpr std::size_t max_workers = 256, queue_capacity = 1024;
    enum class State : std::uint8_t { cold, idle, executing, polling, stopped };
private:
    // SoA: cold OS handles never participate in hot telemetry scans.
    // Columns use cache-line strides to avoid neighboring worker write sharing.
    static constexpr std::size_t state_stride = 64, counter_stride = 8;
    std::array<std::thread, max_workers> threads_{};
    alignas(64) std::array<std::atomic<std::uint8_t>, max_workers * state_stride> states_{};
    alignas(64) std::array<std::atomic<std::uint64_t>, max_workers * counter_stride> completed_{};
    alignas(64) std::atomic<int> poll_owner_{-1};
    // Fixed SoA ready ring; no per-submit heap node or closure allocation.
    std::array<Work, queue_capacity> functions_{};
    std::array<void*, queue_capacity> arguments_{};
    std::mutex gate_, join_gate_;
    std::condition_variable ready_;
    std::size_t head_ = 0, size_ = 0, started_ = 0;
    bool closing_ = false;
    Reactor& reactor_;
    const std::size_t limit_;
    std::atomic<std::uint64_t> failures_{0};
    inline static thread_local SharedReactorPool* current_ = nullptr;

    void state(std::size_t slot, State value) noexcept {
        states_[slot * state_stride].store(static_cast<std::uint8_t>(value), std::memory_order_relaxed);
    }
    void run(std::size_t slot) noexcept {
        current_ = this;
        unsigned slices = 0;
        for (;;) {
            Work work = nullptr;
            void* argument = nullptr;
            {
                std::lock_guard<std::mutex> lock(gate_);
                if (size_) {
                    work = functions_[head_]; argument = arguments_[head_];
                    functions_[head_] = nullptr; arguments_[head_] = nullptr;
                    head_ = (head_ + 1) % queue_capacity; --size_;
                } else if (closing_) break;
            }
            if (work) {
                state(slot, State::executing);
                try { work(argument); } catch (...) { failures_.fetch_add(1, std::memory_order_relaxed); }
                completed_[slot * counter_stride].fetch_add(1, std::memory_order_relaxed);
                if (++slices < 64) continue;
            }
            // Even a permanently full work queue receives an I/O check after
            // every 64 bounded host slices. Handlers themselves must cooperate.
            slices = 0;
            int absent = -1;
            if (poll_owner_.compare_exchange_strong(absent, static_cast<int>(slot),
                    std::memory_order_acquire)) {
                bool may_wait;
                {
                    std::lock_guard<std::mutex> lock(gate_);
                    may_wait = !closing_ && size_ == 0;
                }
                state(slot, State::polling);
                reactor_.poll(may_wait);
                poll_owner_.store(-1, std::memory_order_release);
                ready_.notify_one();
            } else if (!work) {
                state(slot, State::idle);
                std::unique_lock<std::mutex> lock(gate_);
                ready_.wait_for(lock, std::chrono::milliseconds(10),
                        [&] { return closing_ || size_ != 0; });
            }
        }
        state(slot, State::stopped);
        current_ = nullptr;
    }
public:
    explicit SharedReactorPool(Reactor& reactor, std::size_t initial = 2,
                               std::size_t maximum = 2)
        : reactor_(reactor), limit_(maximum) {
        if (!initial || initial > maximum || maximum > max_workers)
            throw std::invalid_argument("worker limits must satisfy 1 <= initial <= maximum <= 256");
        try { ensure_workers(initial); }
        catch (...) { close(); throw; }
    }
    ~SharedReactorPool() { close(); }
    SharedReactorPool(const SharedReactorPool&) = delete;
    SharedReactorPool& operator=(const SharedReactorPool&) = delete;
    // Supervisor-only bounded growth. Actor count / pending I/O do not call it.
    void ensure_workers(std::size_t count) {
        std::lock_guard<std::mutex> lock(gate_);
        if (closing_ || !count || count > limit_) throw std::invalid_argument("worker growth denied");
        while (started_ < count) {
            const auto slot = started_;
            threads_[slot] = std::thread([this, slot] { run(slot); });
            ++started_;
        }
    }
    bool try_submit(Work work, void* argument) {
        if (!work) return false;
        {
            std::lock_guard<std::mutex> lock(gate_);
            if (closing_ || size_ == queue_capacity) return false;
            const auto tail = (head_ + size_) % queue_capacity;
            functions_[tail] = work; arguments_[tail] = argument; ++size_;
        }
        // Publication precedes both wake paths. The reactor wake is sticky,
        // including a producer racing between the empty check and poll entry.
        // Lease acquisition precedes the poller's ready-queue check. If the
        // lease is absent, a future poller observes this published work.
        // Avoid a reactor syscall for every hot-queue submission.
        if (poll_owner_.load(std::memory_order_acquire) != -1) reactor_.wake();
        ready_.notify_one();
        return true;
    }
    std::size_t started_workers() {
        std::lock_guard<std::mutex> lock(gate_); return started_;
    }
    std::uint64_t completed() const noexcept {
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < max_workers; ++i)
            result += completed_[i * counter_stride].load(std::memory_order_relaxed);
        return result;
    }
    std::uint64_t failures() const noexcept { return failures_.load(std::memory_order_relaxed); }
    void close() {
        if (current_ == this) throw std::logic_error("cannot join current worker");
        std::lock_guard<std::mutex> join(join_gate_);
        {
            std::lock_guard<std::mutex> lock(gate_); closing_ = true;
        }
        reactor_.wake(); ready_.notify_all();
        for (std::size_t i = 0; i < started_; ++i)
            if (threads_[i].joinable()) threads_[i].join();
    }
};
} // namespace oreslang::runtime
