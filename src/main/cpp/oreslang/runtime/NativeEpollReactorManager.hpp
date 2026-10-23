#pragma once
#if !defined(__linux__)
#error "NativeEpollReactorManager requires Linux"
#endif
#include "oreslang/runtime/NativeEpollReactor.hpp"
#include <array>
#include <cerrno>
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
        // Host-only teardown. After producer fencing and reactor stop, retire
        // even completions never consumed by the host. No accepted raw watch
        // can leave a process-domain ownership claim permanently stranded.
        {
            std::lock_guard<std::mutex> process_guard(process_claim_mu_);
            for (auto& entry : process_claims_)
                if (entry.occupied && entry.instance == instance_) entry.occupied = false;
        }
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
    // Audit counter: an internal completion that could not be authenticated
    // against a live manager-level ownership claim was suppressed.
    std::uint64_t orphaned_completions() const noexcept {
        return orphaned_completions_.load(std::memory_order_acquire);
    }

    // Legacy/raw descriptors have no authenticated open-file-description
    // provenance. Admit sockets/pipes conservatively and reject anon inodes.
    std::optional<Watch> try_watch(int fd, std::uint32_t events) noexcept {
        return try_watch_impl(fd, events, nullptr);
    }
    // Trusted host calls this before guest work begins. Idempotent and
    // nonparking; caller must observe true (a failed try-lock is NOT a seal).
    // In preallocated RT mode the first valid watch also seals automatically.
    bool seal_owned_bootstrap() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return false;
        owned_bootstrap_sealed_ = true;
        return true;
    }
    // Host bootstrap only: kernel creates a distinct eventfd and this manager
    // mints a private capability. These syscalls are NOT real-time guest work.
    std::optional<OwnedEventFd> open_owned_eventfd() noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_ || owned_bootstrap_sealed_ ||
                next_owner_ == UINT64_MAX ||
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
        if (!guard.owns_lock() || stopped_ || owned_bootstrap_sealed_ ||
                next_generation_ == UINT64_MAX) return std::nullopt;
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
    // Shutdown prohibits new counter writes even for direct manager callers;
    // outstanding pre-stop counters remain drainable for trusted cleanup.
    bool signal_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock() || stopped_) return false;
        const auto* entry = lookup_owned(key);
        if (!entry) return false;
        const std::uint64_t one = 1;
        ssize_t written;
        do {
            written = ::write(entry->fd, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);
        return written == static_cast<ssize_t>(sizeof(one));
    }
    // Trusted host consumer acknowledgement: eventfd readiness persists until
    // its counter is read. This never runs guest callbacks or parks a carrier.
    bool drain_owned_eventfd(OwnedEventFd key) noexcept {
        std::unique_lock<std::mutex> guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return false;
        const auto* entry = lookup_owned(key);
        if (!entry) return false;
        std::uint64_t counter = 0;
        ssize_t received;
        do {
            received = ::read(entry->fd, &counter, sizeof(counter));
        } while (received < 0 && errno == EINTR);
        return received == static_cast<ssize_t>(sizeof(counter));
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
        const int result = ::close(fd);
        const int close_errno = result == 0 ? 0 : errno;
        // A failed close may have invalidated the numeric FD already. Never
        // retry it: another thread could have reused that integer meanwhile.
        entry->live = false; // stale handles cannot act on a closed/uncertain FD
        bool inject_uncertain_close = false;
#if defined(ORESLANG_NIO_TEST_HOOKS)
        // Fault injection occurs AFTER a successful real close, so the test
        // never leaks a descriptor or fabricates ownership of a live FD.
        auto remaining = test_uncertain_close_.load(std::memory_order_relaxed);
        while (remaining && !test_uncertain_close_.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) {}
        inject_uncertain_close = remaining != 0 && result == 0;
#endif
        // On Linux EINTR means the descriptor has been released. An unknown
        // error (notably EBADF) is NOT a reclaim receipt. Quarantine this
        // manager slot instead of making it available to a future mint:
        // the logical authority remains pending for trusted-host escalation.
        if (!inject_uncertain_close &&
                (result == 0 || (result < 0 && close_errno == EINTR)))
            return true;
        entry->quarantined = true;
        return false;
    }
#if defined(ORESLANG_NIO_TEST_HOOKS)
    // Only source-local native test translation units compile these hooks.
    void test_uncertain_next_owned_close(unsigned count = 1) noexcept {
        test_uncertain_close_.store(count, std::memory_order_release);
    }
    std::size_t test_quarantined_owned_slots() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        std::size_t count = 0;
        for (const auto& e : owned_) if (e.quarantined) ++count;
        return count;
    }
#endif
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
        // A tracked manager claim is reserved BEFORE epoll registration.
        // The old fallback attempted to cancel an already accepted ticket if
        // claim capacity unexpectedly ran out. Cancellation is nonparking and
        // may fail under reactor lock contention, stranding an ownerless
        // readiness operation. A fixed-slot pointer is safe under mu_.
        FdClaim* available = nullptr;
        for (auto& claim : claimed_fds_) {
            if (!claim.occupied) {
                if (!available) available = &claim;
                continue;
            }
            if (claim.fd == fd || (owner && claim.owner == owner) ||
                    (stream_identity && claim.stream_identity &&
                     claim.device == identity.st_dev && claim.inode == identity.st_ino &&
                     claim.access_mode == access_mode)) return std::nullopt;
        }
        if (!available) return std::nullopt;
        // Raw sockets and pipes are also subject to a bounded claim domain
        // shared by manager instances in THIS linked runtime image. Acquire
        // it before epoll_ctl, and publish only on successful admission.
        // Never park on a manager/actor hot path; a contended gate rejects.
        std::unique_lock<std::mutex> process_gate(process_claim_mu_,
                                                   std::defer_lock);
        ProcessClaim* process_slot = nullptr;
        if (stream_identity) {
            if (!process_gate.try_lock()) return std::nullopt;
            for (auto& active : process_claims_) {
                if (!active.occupied) {
                    if (!process_slot) process_slot = &active;
                } else if (active.device == identity.st_dev &&
                           active.inode == identity.st_ino &&
                           active.access_mode == access_mode) {
                    return std::nullopt;
                }
            }
            if (!process_slot) return std::nullopt;
        }
        // Tickets returned by each kernel reactor encode only local
        // slot/generation. Two managers can therefore issue identical
        // reactor tokens. Reserve a process-unique public ID BEFORE any
        // epoll registration; exhaustion fails closed without rollback.
        const auto public_id = take_public_ticket();
        if (public_id == 0) return std::nullopt;
        auto count = started_.load(std::memory_order_relaxed);
        // Do not seal a preallocated RT profile merely because validation
        // passed: epoll admission may still fail due to an unsupported FD,
        // per-reactor contention or exhausted capacity. Commit the seal only
        // after a tracked readiness ticket is successfully published.
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
                const Watch result{static_cast<std::uint8_t>(index),
                                   NativeEpollReactor::Ticket{public_id}};
                // Store BOTH ticket namespaces. Only the manager-internal
                // ticket is used to cancel the kernel watch; the public ticket
                // never crosses into the reactor's slot/generation domain.
                *available = FdClaim{true, fd, stream_identity, identity.st_dev,
                                     identity.st_ino, access_mode, result, owner,
                                     *token};
                if (process_slot) {
                    *process_slot = ProcessClaim{true, identity.st_dev,
                        identity.st_ino, access_mode, instance_, public_id};
                    ++active_stream_claims_; // protected by manager mu_
                }
                // The manager lock covers both the ownership table and the
                // bootstrap phase; no creator can race this barrier.
                if (profile_ == Profile::preallocated_rt)
                    owned_bootstrap_sealed_ = true;
                return result;
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
        if (watch.reactor >= count || watch.ticket.value == 0) return false;
        // Check actual live provenance before invoking cancellation: a token
        // created by another manager cannot cancel an unrelated local slot.
        for (const auto& claim : claimed_fds_) {
            if (!claim.occupied || claim.watch.reactor != watch.reactor ||
                    claim.watch.ticket.value != watch.ticket.value) continue;
            return reactors_[watch.reactor]->try_cancel(claim.reactor_ticket);
        }
        return false;
    }

    std::optional<Completion> try_pop() noexcept {
        // Never wait behind producer registration or shutdown on a carrier.
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock()) return std::nullopt;
        // Owned eventfds have manager-validated capability identity and do
        // not enter the global raw-stream table. When this manager has no
        // outstanding raw pipe/socket watches, avoid unrelated process-wide
        // mutex contention on every eventfd completion. The count is guarded
        // by mu_; global stream claims cannot appear while it is held.
        std::unique_lock<std::mutex> process_gate(process_claim_mu_,
                                                   std::defer_lock);
        if (active_stream_claims_ != 0 && !process_gate.try_lock())
            return std::nullopt;
        const auto count = started_.load(std::memory_order_acquire);
        if (count == 0) return std::nullopt;
        const auto first = next_completion_.fetch_add(1, std::memory_order_relaxed) % count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = (first + i) % count;
            auto event = reactors_[index]->try_pop();
            if (event) {
                for (auto& claim : claimed_fds_) {
                    if (!claim.occupied || claim.watch.reactor != index ||
                            claim.reactor_ticket.value != event->ticket.value) continue;
                    // Translate the trusted reactor-local completion to the
                    // globally minted public ticket before exposing the ABI.
                    event->ticket = claim.watch.ticket;
                    if (claim.stream_identity) {
                        for (auto& global : process_claims_) {
                            if (global.occupied && global.instance == instance_ &&
                                global.ticket == event->ticket.value) {
                                global.occupied = false;
                                --active_stream_claims_; // verified global claim
                                break;
                            }
                        }
                    }
                    claim.occupied = false;
                    return Completion{static_cast<std::uint8_t>(index), *event};
                }
                // A completion without a manager claim must not expose a raw
                // reactor token as if it carried valid guest ownership.
                orphaned_completions_.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
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
        NativeEpollReactor::Ticket reactor_ticket{};
    };
    // Bounded manager-local ownership table; stream claims additionally enter
    // a bounded cross-manager table for this linked C++ runtime image only.
    // This does not authenticate cross-DSO or cross-process supervisor FDs.
    static constexpr std::size_t maximum_claims =
        maximum_reactors * NativeEpollReactor::capacity;
    std::array<FdClaim, maximum_claims> claimed_fds_{};
    // Only raw streams need the process-wide lock at terminal retirement.
    // Guarded by mu_ and incremented only with a published global claim.
    std::size_t active_stream_claims_ = 0;
    struct ProcessClaim {
        bool occupied;
        dev_t device;
        ino_t inode;
        int access_mode;
        std::uint64_t instance;
        std::uint64_t ticket;
    };
    // Finite global admission budget: exhaustion rejects without allocating.
    // Instance IDs are non-reused until UINT64_MAX exhaustion.
    inline static std::mutex process_claim_mu_{};
    inline static std::array<ProcessClaim, maximum_claims * 4> process_claims_{};
    static constexpr std::size_t owned_capacity = 256;
    struct OwnedEntry {
        bool live = false;
        int fd = -1;
        std::uint64_t owner = 0;
        std::uint64_t generation = 0;
        // Unknown close outcome: never reuse this slot or retry the fd.
        bool quarantined = false;
    };
    std::size_t free_owned_slot() const noexcept {
        for (std::size_t i = 0; i < owned_capacity; ++i)
            if (!owned_[i].live && !owned_[i].quarantined) return i;
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
    // Lifetime-unique within this linked runtime image; never wrap into a
    // formerly issued handle, even if a watch admission was rejected.
    static std::uint64_t take_public_ticket() noexcept {
        auto id = global_public_ticket_.load(std::memory_order_relaxed);
        while (id != UINT64_MAX && id != 0) {
            if (global_public_ticket_.compare_exchange_weak(
                    id, id + 1, std::memory_order_relaxed))
                return id;
        }
        return 0; // fail closed at exhaustion
    }
    inline static std::atomic<std::uint64_t> global_public_ticket_{1};
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
    std::atomic<std::uint64_t> orphaned_completions_{0};
    mutable std::mutex mu_;
    bool stopped_ = false;
    bool owned_bootstrap_sealed_ = false; // guarded by mu_
#if defined(ORESLANG_NIO_TEST_HOOKS)
    std::atomic<unsigned> test_uncertain_close_{0};
#endif
};
} // namespace oreslang::runtime
