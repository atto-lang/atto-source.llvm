#pragma once
#if !defined(__linux__)
#error "NativeEpollReactor requires Linux"
#endif
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace oreslang::runtime {

/**
 * One dedicated kernel-readiness thread, not an actor carrier. At most 64
 * outstanding one-shot readiness tickets; all records and notifications are
 * preallocated before the reactor starts. No guest callbacks run here.
 *
 * The submitting host retains its file descriptor until the terminal record
 * is consumed; guest/actor capability ownership is not implemented here.
 * Only host/control code may start/stop or watch descriptors directly.
 */
class NativeEpollReactor final {
public:
    static constexpr std::size_t capacity = 64;
    static constexpr std::uint64_t wake_token = UINT64_MAX;
    struct Ticket { std::uint64_t value = 0; };
    enum class Outcome : std::uint8_t { ready, cancelled, stopped };
    struct Completion { Ticket ticket; int fd; std::uint32_t events; Outcome outcome; };

    NativeEpollReactor() {
        epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
        wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_ < 0) {
            const int saved = errno;
            cleanup();
            throw std::system_error(saved, std::generic_category(), "eventfd wakeup");
        }
        completion_signal_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (completion_signal_ < 0) {
            // Preserve the failing syscall's errno across close(): cleanup
            // may otherwise overwrite the actionable startup diagnosis.
            const int saved = errno;
            cleanup();
            throw std::system_error(saved, std::generic_category(), "eventfd completion");
        }
        epoll_event e{};
        e.events = EPOLLIN;
        e.data.u64 = wake_token;
        if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, wake_, &e) != 0) {
            int saved = errno;
            cleanup();
            throw std::system_error(saved, std::generic_category(), "epoll_ctl wakeup");
        }
    }

    NativeEpollReactor(const NativeEpollReactor&) = delete;
    NativeEpollReactor& operator=(const NativeEpollReactor&) = delete;
    ~NativeEpollReactor() noexcept { stop(); cleanup(); }

    bool start() {
        std::lock_guard<std::mutex> lock(mu_); // host bootstrap only
        if (closed_.load(std::memory_order_acquire)) return false;
        if (started_) return true;
        try { thread_ = std::thread(&NativeEpollReactor::loop, this); }
        catch (const std::system_error&) { return false; }
        started_ = true;
        return true;
    }

    /** Never called on a reactor's own thread; runtime does not expose callbacks. */
    void stop() noexcept {
        // Only one host teardown caller may examine/join std::thread. The
        // reactor state mutex alone cannot protect joinable()/join() because
        // it must be released while the worker drains and exits.
        std::lock_guard<std::mutex> teardown_lock(stop_mu_);
        // Linearize shutdown against cancellation/readiness ownership. Once
        // closed_ is published under mu_, no later cancellation may convert a
        // shutdown-fenced watch into a successful cancelled completion.
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_.store(true, std::memory_order_release);
        }
        signal(wake_);
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) std::terminate();
            thread_.join(); // host teardown only, never actor/CONTROL turn
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& item = slots_[i];
            if (item.state != State::active) continue;
            (void)::epoll_ctl(epoll_, EPOLL_CTL_DEL, item.fd, nullptr);
            item.state = State::completed;
            emit_locked({Ticket{encode(i, item.generation)}, item.fd, 0, Outcome::stopped});
        }
    }

    /** Validate an admission without starting a reactor thread. This lets the
     * throughput manager reject malformed/blocking descriptors while remaining
     * at zero pthreads until a real readiness watch is accepted.
     */
    static bool can_watch(int fd, std::uint32_t events) noexcept {
        if (fd < 0 || !valid_events(events)) return false;
        const int flags = ::fcntl(fd, F_GETFL);
        return flags >= 0 && (flags & O_NONBLOCK) != 0;
    }

    /** Immediate nonparking host admission; lock contention returns no ticket. */
    std::optional<Ticket> try_watch(int fd, std::uint32_t events) noexcept {
        if (!can_watch(fd, events) || closed_.load(std::memory_order_acquire))
            return std::nullopt;
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || !started_ || closed_.load(std::memory_order_relaxed))
            return std::nullopt;
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& item = slots_[i];
            if (item.state != State::free || item.generation >= UINT32_MAX - 1) continue;
            const auto new_generation = item.generation + 1;
            const auto token = encode(i, new_generation);
            epoll_event e{};
            e.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
            e.data.u64 = token;
            if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e) != 0) return std::nullopt;
            item = Slot{fd, new_generation, State::active};
            pending_.fetch_add(1, std::memory_order_relaxed);
            return Ticket{token};
        }
        return std::nullopt;
    }

    /** Cancellation races readiness under one short lock; exactly one wins. */
    bool try_cancel(Ticket ticket) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || closed_.load(std::memory_order_relaxed)) return false;
        Slot* item = lookup_active(ticket);
        if (!item) return false;
        (void)::epoll_ctl(epoll_, EPOLL_CTL_DEL, item->fd, nullptr);
        item->state = State::completed;
        emit_locked({ticket, item->fd, 0, Outcome::cancelled});
        return true;
    }

    /** Nonparking consumption. A consumed ticket's slot can be reused. */
    std::optional<Completion> try_pop() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || completion_count_ == 0) return std::nullopt;
        auto result = completions_[completion_head_];
        completion_head_ = (completion_head_ + 1) % capacity;
        --completion_count_;
        completion_pending_.store(completion_count_, std::memory_order_release);
        const std::size_t index = decode_index(result.ticket.value);
        // Uniqueness is guaranteed by the slot being retained until pop.
        if (index < capacity && slots_[index].generation == decode_generation(result.ticket.value)
                && slots_[index].state == State::completed) {
            slots_[index].state = State::free;
            pending_.fetch_sub(1, std::memory_order_relaxed);
        }
        return result;
    }

    /** Scheduler/CONTROL can watch this eventfd for completion publication. */
    int completion_fd() const noexcept { return completion_signal_; }
    void drain_completion_signal() noexcept {
        std::uint64_t value;
        while (::read(completion_signal_, &value, sizeof(value)) == sizeof(value)) { }
        // A nonparking try_pop() may lose its try-lock race after an outer
        // poller acknowledges eventfd. Re-arm while records are still queued
        // so the host cannot sleep indefinitely with unread completions.
        // A concurrent pop may cause one harmless extra wakeup, never a
        // lost notification or an unbounded busy wait.
        if (completion_pending_.load(std::memory_order_acquire) != 0)
            signal(completion_signal_);
    }
    std::size_t pending() const noexcept { return pending_.load(std::memory_order_relaxed); }
    std::uint64_t events_processed() const noexcept { return events_processed_.load(std::memory_order_relaxed); }
    bool is_started() const noexcept { return started_; } // inspect at bootstrap/after join only

private:
    enum class State : std::uint8_t { free, active, completed };
    struct Slot { int fd = -1; std::uint32_t generation = 0; State state = State::free; };
    static constexpr std::size_t batch = 32;
    static bool valid_events(std::uint32_t events) noexcept {
        return (events & (EPOLLIN | EPOLLOUT)) != 0
            && (events & ~(static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT))) == 0;
    }
    static std::uint64_t encode(std::size_t slot, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | (slot + 1);
    }
    static std::size_t decode_index(std::uint64_t token) noexcept {
        const auto raw = static_cast<std::uint32_t>(token);
        return raw == 0 ? capacity : raw - 1;
    }
    static std::uint32_t decode_generation(std::uint64_t token) noexcept {
        return static_cast<std::uint32_t>(token >> 32);
    }
    Slot* lookup_active(Ticket ticket) noexcept {
        const std::size_t index = decode_index(ticket.value);
        if (index >= capacity) return nullptr;
        auto& item = slots_[index];
        if (item.generation != decode_generation(ticket.value) || item.state != State::active)
            return nullptr;
        return &item;
    }
    static void signal(int fd) noexcept {
        if (fd < 0) return;
        const std::uint64_t one = 1;
        // GCC's warn_unused_result applies even to an explicit (void) cast.
        // eventfd is O_NONBLOCK: EAGAIN means a wakeup is already pending.
        // Retry only interrupted syscalls, never wait for notification space.
        ssize_t written;
        do { written = ::write(fd, &one, sizeof(one)); }
        while (written == -1 && errno == EINTR);
        if (written == static_cast<ssize_t>(sizeof(one))
            || (written == -1 && errno == EAGAIN)) return;
        // A failed host notification must never call guest code or block here.
        // Shutdown/terminal cleanup also publishes a completion record.
    }
    void emit_locked(Completion completion) noexcept {
        // Each ACTIVE slot can emit exactly one completion before it is freed,
        // so this preallocated queue cannot overflow under the admission cap.
        if (completion_count_ >= capacity) std::terminate();
        const auto index = (completion_head_ + completion_count_) % capacity;
        completions_[index] = completion;
        ++completion_count_;
        completion_pending_.store(completion_count_, std::memory_order_release);
        signal(completion_signal_);
    }
    void loop() noexcept {
        std::array<epoll_event, batch> ready{};
        while (!closed_.load(std::memory_order_acquire)) {
            const int count = ::epoll_wait(epoll_, ready.data(), static_cast<int>(ready.size()), -1);
            if (count < 0) { if (errno == EINTR) continue; break; }
            for (int i = 0; i < count; ++i) {
                const auto token = ready[static_cast<std::size_t>(i)].data.u64;
                if (token == wake_token) {
                    std::uint64_t value;
                    while (::read(wake_, &value, sizeof(value)) == sizeof(value)) { }
                    continue;
                }
                std::lock_guard<std::mutex> lock(mu_); // reactor-only wait is permitted
                // stop() uses the same lock to publish closed_. If shutdown
                // wins this lock, terminal cleanup owns the watch and must
                // publish stopped rather than a late ready result.
                if (closed_.load(std::memory_order_relaxed)) break;
                Slot* item = lookup_active(Ticket{token});
                if (!item) continue; // stale event, cancelled slot or FD reuse
                (void)::epoll_ctl(epoll_, EPOLL_CTL_DEL, item->fd, nullptr);
                item->state = State::completed;
                emit_locked({Ticket{token}, item->fd,
                    ready[static_cast<std::size_t>(i)].events, Outcome::ready});
                events_processed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // A terminal epoll failure must not strand accepted operations.
        // Publish exactly one stopped result for each outstanding ticket even
        // when no external host subsequently calls stop().
        std::lock_guard<std::mutex> terminal_lock(mu_);
        // Fatal epoll termination has the same admission/cancellation fence
        // as explicit stop(). No late cancellation can win after this state
        // transition is published.
        closed_.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& item = slots_[i];
            if (item.state != State::active) continue;
            (void)::epoll_ctl(epoll_, EPOLL_CTL_DEL, item.fd, nullptr);
            item.state = State::completed;
            emit_locked({Ticket{encode(i, item.generation)}, item.fd, 0, Outcome::stopped});
        }
    }
    void cleanup() noexcept {
        if (completion_signal_ >= 0) { ::close(completion_signal_); completion_signal_ = -1; }
        if (wake_ >= 0) { ::close(wake_); wake_ = -1; }
        if (epoll_ >= 0) { ::close(epoll_); epoll_ = -1; }
    }

    int epoll_ = -1;
    int wake_ = -1;
    int completion_signal_ = -1;
    std::atomic<bool> closed_{false};
    std::thread thread_;
    bool started_ = false;
    // Separate host lifecycle serialization so two stop() callers never race
    // std::thread::joinable()/join() or cleanup the same join handle.
    std::mutex stop_mu_;
    mutable std::mutex mu_;
    std::array<Slot, capacity> slots_{};
    std::array<Completion, capacity> completions_{};
    std::size_t completion_head_ = 0;
    std::size_t completion_count_ = 0;
    std::atomic<std::size_t> completion_pending_{0};
    std::atomic<std::size_t> pending_{0};
    std::atomic<std::uint64_t> events_processed_{0};
};
} // namespace oreslang::runtime
