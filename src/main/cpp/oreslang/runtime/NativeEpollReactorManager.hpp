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
#include <sys/eventfd.h>
#include <unistd.h>
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
    // Opaque trusted host-only descriptor authority. Not representable in the
    // existing C/guest ABI, and stale generations fail after retirement.
    class OwnedEventFd final {
        friend class NativeEpollReactorManager;
        std::uint64_t instance_ = 0;
        std::uint64_t generation_ = 0;
        std::uint16_t slot_ = 0;
        OwnedEventFd(std::uint64_t instance, std::uint64_t generation,
                     std::uint16_t slot) noexcept
            : instance_(instance), generation_(generation), slot_(slot) {}
    public:
        OwnedEventFd() noexcept = default;
    };

    explicit NativeEpollReactorManager(std::size_t limit = maximum_reactors,
                                      Profile profile = Profile::throughput,
                                      std::size_t rt_reactors = 1)
        : limit_(limit), profile_(profile), instance_(take_instance()) {
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
    ~NativeEpollReactorManager() noexcept {
        stop();
        // Trusted producers must be fenced before destroying the manager.
        for (auto& entry : owned_) {
            if (!entry.live) continue;
            (void)::close(entry.fd); // never retry close: possible FD reuse
            entry.live = false;
        }
    }

    std::size_t started_threads() const noexcept { return started_.load(std::memory_order_acquire); }
    std::size_t limit() const noexcept { return limit_; }
    Profile profile() const noexcept { return profile_; }

    // Legacy/raw descriptors have no authenticated open-file-description
    // provenance. Admit sockets/pipes conservatively and reject anon inodes.
    std::optional<Watch> try_watch(int fd, std::uint32_t events) noexcept {
        return try_watch_impl(fd, events, nullptr);
    }
    // Host bootstrap only: kernel creates a distinct eventfd and this manager
    // mints a private capability. These syscalls are NOT real-time guest work.
    std::optional<OwnedEventFd> open_owned_eventfd() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || next_owner_ == UINT64_MAX ||
                next_generation_ == UINT64_MAX) return std::nullopt;
        const auto index = free_owned_slot();
        if (index == owned_capacity) return std::nullopt;
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) return std::nullopt;
        auto& entry = owned_[index];
        entry = OwnedEntry{true, fd, next_owner_++, next_generation_++};
        return OwnedEventFd{instance_, entry.generation,
                            static_cast<std::uint16_t>(index)};
    }
    // Host-only kernel dup: aliases inherit exactly the same owner ID.
    std::optional<OwnedEventFd> duplicate_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || next_generation_ == UINT64_MAX)
            return std::nullopt;
        const auto* source = lookup_owned(key);
        if (!source) return std::nullopt;
        const auto index = free_owned_slot();
        if (index == owned_capacity) return std::nullopt;
        const int fd = ::fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) return std::nullopt;
        auto& entry = owned_[index];
        entry = OwnedEntry{true, fd, source->owner, next_generation_++};
        return OwnedEventFd{instance_, entry.generation,
                            static_cast<std::uint16_t>(index)};
    }
    // Trusted host signal, never invoked on guest/control execution lanes.
    bool signal_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return false;
        const auto* entry = lookup_owned(key);
        if (!entry) return false;
        const std::uint64_t one = 1;
        return ::write(entry->fd, &one, sizeof(one)) ==
               static_cast<ssize_t>(sizeof(one));
    }
    // Trusted host consumer acknowledgement: eventfd readiness persists until
    // its counter is read. This never runs guest callbacks or parks a carrier.
    bool drain_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return false;
        const auto* entry = lookup_owned(key);
        if (!entry) return false;
        std::uint64_t counter = 0;
        return ::read(entry->fd, &counter, sizeof(counter)) ==
               static_cast<ssize_t>(sizeof(counter));
    }
    // Terminal completion must be consumed before the watched handle closes.
    bool close_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return false;
        auto* entry = lookup_owned(key);
        if (!entry) return false;
        for (const auto& claim : claimed_fds_)
            if (claim.occupied && claim.fd == entry->fd) return false;
        const int fd = entry->fd;
        entry->live = false; // stale handles may never authorize FD reuse
        return ::close(fd) == 0;
    }
    // Typed host-only watch. No guest or raw C ABI can forge its owner ID.
    std::optional<Watch> try_watch(OwnedEventFd key, std::uint32_t events) noexcept {
        return try_watch_impl(-1, events, &key);
    }

private:
    std::optional<Watch> try_watch_impl(int fd, std::uint32_t events,
                                        const OwnedEventFd* key) noexcept {
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock() || stopped_) return std::nullopt;
        std::uint64_t owner = 0;
        if (key) {
            const auto* entry = lookup_owned(*key);
            if (!entry) return std::nullopt;
            fd = entry->fd;
            owner = entry->owner;
        }
        if (!NativeEpollReactor::can_watch(fd, events)) return std::nullopt;
        struct stat identity{};
        if (::fstat(fd, &identity) != 0) return std::nullopt;
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0) return std::nullopt;
        const int access_mode = flags & O_ACCMODE;
        const bool stream_identity = S_ISFIFO(identity.st_mode) || S_ISSOCK(identity.st_mode);
        if (!key && !stream_identity) return std::nullopt;
        // Reject aliases before lazy host reactor-thread provisioning.
        for (const auto& claim : claimed_fds_) {
            if (!claim.occupied) continue;
            if (claim.fd == fd || (owner && claim.owner == owner) ||
                    (stream_identity && claim.stream_identity &&
                     claim.device == identity.st_dev && claim.inode == identity.st_ino &&
                     claim.access_mode == access_mode)) return std::nullopt;
        }
        auto count = started_.load(std::memory_order_relaxed);
        if (count == 0) {
            if (profile_ != Profile::throughput || !reactors_[0]->start())
                return std::nullopt;
            started_.store(1, std::memory_order_release);
            count = 1;
        }
        maybe_expand_locked();
        count = started_.load(std::memory_order_relaxed);
        std::size_t best = 0;
        for (std::size_t i = 1; i < count; ++i)
            if (reactors_[i]->pending() < reactors_[best]->pending()) best = i;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t index = (best + i) % count;
            auto token = reactors_[index]->try_watch(fd, events);
            if (token) {
                const Watch result{static_cast<std::uint8_t>(index), *token};
                for (auto& claim : claimed_fds_) {
                    if (claim.occupied) continue;
                    claim = FdClaim{true, fd, stream_identity, identity.st_dev,
                                    identity.st_ino, access_mode, result, owner};
                    return result;
                }
                (void)reactors_[index]->try_cancel(*token);
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

public:
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
        std::uint64_t owner = 0;
    };
    // Fixed-size, process-local ownership table. No allocation on reactor
    // registration/completion paths; protected by mu_ with try-lock admission.
    static constexpr std::size_t maximum_claims =
        maximum_reactors * NativeEpollReactor::capacity;
    std::array<FdClaim, maximum_claims> claimed_fds_{};
    static constexpr std::size_t owned_capacity = 256;
    struct OwnedEntry {
        bool live = false;
        int fd = -1;
        std::uint64_t owner = 0;
        std::uint64_t generation = 0;
    };
    std::size_t free_owned_slot() const noexcept {
        for (std::size_t i = 0; i < owned_capacity; ++i)
            if (!owned_[i].live) return i;
        return owned_capacity;
    }
    OwnedEntry* lookup_owned(OwnedEventFd key) noexcept {
        if (key.instance_ != instance_ || !key.generation_ ||
                key.slot_ >= owned_capacity) return nullptr;
        auto& entry = owned_[key.slot_];
        return entry.live && entry.generation == key.generation_ ? &entry : nullptr;
    }
    static std::uint64_t take_instance() {
        auto value = global_instance_.load(std::memory_order_relaxed);
        while (value != UINT64_MAX && value != 0) {
            if (global_instance_.compare_exchange_weak(value, value + 1,
                    std::memory_order_relaxed)) return value;
        }
        throw std::runtime_error("native NIO capability instance exhausted");
    }
    inline static std::atomic<std::uint64_t> global_instance_{1};
    const std::size_t limit_;
    const Profile profile_;
    const std::uint64_t instance_;
    std::uint64_t next_owner_ = 1;
    std::uint64_t next_generation_ = 1;
    std::array<OwnedEntry, owned_capacity> owned_{};
    std::array<std::unique_ptr<NativeEpollReactor>, maximum_reactors> reactors_{};
    std::atomic<std::size_t> started_{0};
    std::array<std::uint64_t, maximum_reactors> last_sample_{};
    std::atomic<std::size_t> next_completion_{0};
    mutable std::mutex mu_;
    bool stopped_ = false;
};
} // namespace oreslang::runtime
