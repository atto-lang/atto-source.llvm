#pragma once
#if !defined(__linux__)
#error "NativeEpollReactorManager requires Linux"
#endif
#include "oreslang/runtime/NativeEpollReactor.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>

namespace oreslang::runtime {

/**
 * Bounded host reactor domain. Reactor objects/FDs are allocated during
 * manager construction, but normal throughput starts with zero epoll threads.
 * The first valid readiness watch starts reactor #0; sustained event processing
 * may later add reactors up to the configured bound.
 * RT-preallocated mode starts its fixed thread set before any guest work;
 * no runtime thread provisioning is permitted in that mode.
 *
 * No guest callbacks, heap allocation, or blocking waits in try_watch,
 * try_cancel, or try_pop. Each completion is an opaque readiness record;
 * the VM/actor scheduler is responsible for resolving the owning Future.
 */
class NativeEpollReactorManager final {
public:
    static constexpr std::size_t maximum_reactors = 4;
    enum class Profile : std::uint8_t { throughput, preallocated_rt };
    struct Watch { std::uint8_t reactor; NativeEpollReactor::Ticket ticket; };
    struct Completion { std::uint8_t reactor; NativeEpollReactor::Completion event; };

    explicit NativeEpollReactorManager(std::size_t limit = maximum_reactors,
                                      Profile profile = Profile::throughput,
                                      std::size_t rt_reactors = 1)
        : limit_(limit), profile_(profile) {
        if (limit == 0 || limit > maximum_reactors || rt_reactors == 0 || rt_reactors > limit)
            throw std::invalid_argument("invalid native NIO reactor configuration");
        // All selector objects and completion rings are established before
        // a single scheduler/guest turn may be admitted.
        for (std::size_t i = 0; i < limit_; ++i)
            reactors_[i] = std::make_unique<NativeEpollReactor>();
        const std::size_t required =
            profile_ == Profile::preallocated_rt ? rt_reactors : 0;
        for (std::size_t i = 0; i < required; ++i) {
            if (!reactors_[i]->start()) {
                stop(); // constructor bootstrap failure; no guest work yet
                throw std::runtime_error("native NIO reactor pthread creation failed");
            }
            ++started_;
        }
    }

    NativeEpollReactorManager(const NativeEpollReactorManager&) = delete;
    NativeEpollReactorManager& operator=(const NativeEpollReactorManager&) = delete;
    ~NativeEpollReactorManager() noexcept { stop(); }

    std::size_t started_threads() const noexcept { return started_.load(std::memory_order_acquire); }
    std::size_t limit() const noexcept { return limit_; }
    Profile profile() const noexcept { return profile_; }

    /** Bounded nonparking read/write/accept readiness admission. */
    std::optional<Watch> try_watch(int fd, std::uint32_t events) noexcept {
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock() || stopped_) return std::nullopt;
        // Invalid/blocking descriptors must not cause cold-start thread
        // creation. Validate before provisioning the first throughput reactor.
        if (!NativeEpollReactor::can_watch(fd, events)) return std::nullopt;
        // dup() aliases of sockets/pipes refer to the same kernel stream
        // despite different integer descriptors. Resolve identity before any
        // cold-start thread provisioning; malformed/closed FDs fail closed.
        struct stat identity{};
        if (::fstat(fd, &identity) != 0) return std::nullopt;
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0) return std::nullopt;
        const int access_mode = flags & O_ACCMODE;
        const bool stream_identity = S_ISFIFO(identity.st_mode) || S_ISSOCK(identity.st_mode);
        auto count = started_.load(std::memory_order_relaxed);
        if (count == 0) {
            if (profile_ != Profile::throughput || !reactors_[0]->start())
                return std::nullopt;
            started_.store(1, std::memory_order_release);
            count = 1;
        }
        // A literal FD or dup() alias must have one outstanding owner.
        // Separate pipe read/write ends share st_dev/st_ino but differ in
        // access mode, so they remain independently watchable. Linux anon-inode
        // eventfds can report identical st_dev/st_ino despite being unrelated:
        // use numeric-FD claims for those and require a stronger upstream
        // capability boundary if alias-level ownership is necessary.
        for (const auto& claim : claimed_fds_) {
            if (!claim.occupied) continue;
            if (claim.fd == fd || (stream_identity && claim.stream_identity
                    && claim.device == identity.st_dev
                    && claim.inode == identity.st_ino
                    && claim.access_mode == access_mode)) return std::nullopt;
        }
        maybe_expand_locked();
        count = started_.load(std::memory_order_relaxed);
        std::size_t best = 0;
        for (std::size_t i = 1; i < count; ++i) {
            if (reactors_[i]->pending() < reactors_[best]->pending()) best = i;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t index = (best + i) % count;
            auto token = reactors_[index]->try_watch(fd, events);
            if (token) {
                const Watch result{static_cast<std::uint8_t>(index), *token};
                for (auto& claim : claimed_fds_) {
                    if (claim.occupied) continue;
                    claim = FdClaim{true, fd, stream_identity,
                                    identity.st_dev, identity.st_ino,
                                    access_mode, result};
                    return result;
                }
                // With per-reactor capacity and one claim per active slot this
                // must be unreachable. Revoke rather than expose an untracked
                // readiness token if an internal accounting invariant breaks.
                (void)reactors_[index]->try_cancel(*token);
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool try_cancel(Watch watch) noexcept {
        // Match registration and completion's manager-level retirement fence.
        // A cancellation must never overtake stop() and turn a pending
        // shutdown completion into a spurious successful cancellation.
        // The try-lock does not park an actor or deadline-sensitive carrier.
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock() || stopped_) return false;
        const auto count = started_.load(std::memory_order_acquire);
        if (watch.reactor >= count) return false;
        return reactors_[watch.reactor]->try_cancel(watch.ticket);
    }

    std::optional<Completion> try_pop() noexcept {
        // Never wait behind producer registration or shutdown on a carrier.
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock()) return std::nullopt;
        const auto count = started_.load(std::memory_order_acquire);
        if (count == 0) return std::nullopt;
        const auto first = next_completion_.fetch_add(1, std::memory_order_relaxed) % count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = (first + i) % count;
            auto event = reactors_[index]->try_pop();
            if (event) {
                for (auto& claim : claimed_fds_) {
                    if (!claim.occupied || claim.watch.reactor != index
                        || claim.watch.ticket.value != event->ticket.value) continue;
                    claim.occupied = false;
                    break;
                }
                return Completion{static_cast<std::uint8_t>(index), *event};
            }
        }
        return std::nullopt;
    }

    int completion_fd(std::size_t index) const noexcept {
        return index < started_.load(std::memory_order_acquire)
            ? reactors_[index]->completion_fd() : -1;
    }

    /** A completion-fd consumer drains eventfd readiness before polling records. */
    void drain_completion_signal(std::size_t index) noexcept {
        if (index < started_.load(std::memory_order_acquire))
            reactors_[index]->drain_completion_signal();
    }

    /** Host teardown only. Joining reactor threads may wait for OS scheduling. */
    void stop() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return;
        stopped_ = true;
        for (std::size_t i = 0; i < started_.load(std::memory_order_relaxed); ++i)
            reactors_[i]->stop();
    }

private:
    void maybe_expand_locked() noexcept {
        if (profile_ == Profile::preallocated_rt) return;
        const std::size_t count = started_.load(std::memory_order_relaxed);
        if (count >= limit_) return;
        // Only sustained processed events AND a saturated active readiness
        // population trigger expansion. Idle pending I/O by itself does not.
        for (std::size_t i = 0; i < count; ++i) {
            const auto processed = reactors_[i]->events_processed();
            if (processed - last_sample_[i] < 128
                    || reactors_[i]->pending() < NativeEpollReactor::capacity / 2)
                continue;
            if (!reactors_[count]->start()) return; // bounded resource failure
            ++started_;
            last_sample_[i] = processed;
            return;
        }
    }

    struct FdClaim {
        bool occupied = false;
        int fd = -1;
        bool stream_identity = false;
        dev_t device = 0;
        ino_t inode = 0;
        int access_mode = 0;
        Watch watch{};
    };
    // Fixed-size, process-local ownership table. No allocation on reactor
    // registration/completion paths; protected by mu_ with try-lock admission.
    static constexpr std::size_t maximum_claims =
        maximum_reactors * NativeEpollReactor::capacity;
    std::array<FdClaim, maximum_claims> claimed_fds_{};
    const std::size_t limit_;
    const Profile profile_;
    std::array<std::unique_ptr<NativeEpollReactor>, maximum_reactors> reactors_{};
    std::atomic<std::size_t> started_{0};
    std::array<std::uint64_t, maximum_reactors> last_sample_{};
    std::atomic<std::size_t> next_completion_{0};
    mutable std::mutex mu_;
    bool stopped_ = false;
};
} // namespace oreslang::runtime
