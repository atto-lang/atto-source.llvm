#pragma once
#if !defined(__linux__)
#error "NativeEpollReactorManager requires Linux"
#endif
#include "oreslang/runtime/NativeEpollReactor.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>

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
    struct ActorWatch { Watch watch; NativeActorRef actor; };
    struct Completion {
        std::uint8_t reactor;
        NativeEpollReactor::Completion event;
        // Present only for actor-owned watches. The reactor loan remains
        // charged until ack_actor_completion() transfers/abandons this
        // terminal record. Numeric actor IDs are never sufficient authority.
        std::optional<NativeActorRef> actor;
    };

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
    ~NativeEpollReactorManager() noexcept {
        stop();
        abandon_actor_claims();
    }

    std::size_t started_threads() const noexcept { return started_.load(std::memory_order_acquire); }
    std::size_t limit() const noexcept { return limit_; }
    Profile profile() const noexcept { return profile_; }

    /** Bounded nonparking read/write/accept readiness admission. */
    std::optional<Watch> try_watch(int fd, std::uint32_t events) noexcept {
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock()) return std::nullopt;
        return try_watch_locked(fd, events, nullptr);
    }

    /**
     * Actor-incarnation-bound readiness admission. Retaining the actor loan
     * linearizes against STOP/close/finalization: if actor admission wins,
     * DONE cannot settle until this watch's terminal record is explicitly
     * acknowledged. If STOP wins, no epoll registration occurs.
     */
    std::optional<ActorWatch> try_watch_actor(
            const NativeActorRef& actor, int fd, std::uint32_t events) noexcept {
        if (!NativeEpollReactor::can_watch(fd, events)) return std::nullopt;
        try {
            actor.retain_resource_loan();
        } catch (...) {
            return std::nullopt;
        }
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock()) {
            actor.release_resource_loan();
            return std::nullopt;
        }
        // Dup/inode alias fencing runs inside try_watch_locked, under mu_,
        // for both host and actor-owned registrations.
        auto watch = try_watch_locked(fd, events, &actor);
        if (!watch) {
            actor.release_resource_loan();
            return std::nullopt;
        }
        return ActorWatch{*watch, actor};
    }

    bool try_cancel(Watch watch) noexcept {
        // Match registration and completion's manager-level retirement fence.
        // A cancellation must never overtake stop() and turn a pending
        // shutdown completion into a spurious successful cancellation.
        // The try-lock does not park an actor or deadline-sensitive carrier.
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock() || stopped_) return false;
        FdClaim* claim = find_claim_locked(watch);
        // Actor-owned registrations require the exact incarnation capability;
        // a bare numeric reactor ticket is not cancellation authority.
        if (!claim || claim->actor) return false;
        const auto count = started_.load(std::memory_order_acquire);
        if (watch.reactor >= count) return false;
        return reactors_[watch.reactor]->try_cancel(watch.ticket);
    }

    bool try_cancel_actor(const ActorWatch& watch) noexcept {
        std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
        if (!gate.owns_lock() || stopped_) return false;
        FdClaim* claim = find_claim_locked(watch.watch);
        if (!claim || !claim->actor
                || !claim->actor->same_incarnation(watch.actor))
            return false;
        const auto count = started_.load(std::memory_order_acquire);
        if (watch.watch.reactor >= count) return false;
        return reactors_[watch.watch.reactor]->try_cancel(watch.watch.ticket);
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
            if (!event) continue;
            std::optional<NativeActorRef> actor;
            for (auto& claim : claimed_fds_) {
                if (!claim.occupied || claim.watch.reactor != index
                    || claim.watch.ticket.value != event->ticket.value) continue;
                actor = claim.actor;
                if (actor) {
                    // Keep the FD claim and actor loan alive until trusted
                    // scheduler code has transferred this completion into its
                    // continuation/wake escrow and acknowledges that transfer.
                    claim.completion_delivered = true;
                } else {
                    claim = FdClaim{};
                }
                break;
            }
            return Completion{static_cast<std::uint8_t>(index), *event, actor};
        }
        return std::nullopt;
    }

    /**
     * Complete the reactor-to-scheduler ownership handoff. For actor-owned
     * terminal records, call only after any guest continuation has acquired
     * its own generation-fenced wake/continuation escrow. This releases the
     * reactor's one resource loan exactly once and allows FD reuse.
     */
    bool ack_actor_completion(const Completion& completion) noexcept {
        if (!completion.actor) return false;
        std::optional<NativeActorRef> release;
        {
            std::unique_lock<std::mutex> gate(mu_, std::try_to_lock);
            if (!gate.owns_lock()) return false;
            const Watch watch{completion.reactor, completion.event.ticket};
            FdClaim* claim = find_claim_locked(watch);
            if (!claim || !claim->actor || !claim->completion_delivered
                    || !claim->actor->same_incarnation(*completion.actor))
                return false;
            release = claim->actor;
            *claim = FdClaim{};
        }
        release->release_resource_loan();
        return true;
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
        Watch watch{};
        std::optional<NativeActorRef> actor;
        bool completion_delivered = false;
        // Physical transport identity, so dup()/dup2() aliases of a claimed
        // socket or same-direction pipe end cannot register a second owner.
        dev_t device = 0;
        ino_t inode = 0;
        mode_t mode = 0;
        int access_mode = 0;
    };

    FdClaim* find_claim_locked(Watch watch) noexcept {
        for (auto& claim : claimed_fds_) {
            if (claim.occupied
                    && claim.watch.reactor == watch.reactor
                    && claim.watch.ticket.value == watch.ticket.value)
                return &claim;
        }
        return nullptr;
    }

    std::optional<Watch> try_watch_locked(
            int fd, std::uint32_t events, const NativeActorRef* actor) noexcept {
        if (stopped_) return std::nullopt;
        // Invalid/blocking descriptors must not cause cold-start thread
        // creation. Validate before provisioning the first throughput reactor.
        if (!NativeEpollReactor::can_watch(fd, events)) return std::nullopt;
        auto count = started_.load(std::memory_order_relaxed);
        if (count == 0) {
            if (profile_ != Profile::throughput || !reactors_[0]->start())
                return std::nullopt;
            started_.store(1, std::memory_order_release);
            count = 1;
        }
        // Distinct dup()/dup2() FD values may refer to the same socket/pipe.
        // Conservatively fence an equal Linux transport inode identity.
        // No allocation, guest pointer, or mutable buffer access here.
        struct stat identity{};
        if (::fstat(fd, &identity) != 0) return std::nullopt;
        const bool transport = S_ISSOCK(identity.st_mode) || S_ISFIFO(identity.st_mode);
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0) return std::nullopt;
        const int access_mode = flags & O_ACCMODE;
        // One FD can only have one outstanding readiness owner in this
        // process-level manager, even if two different epoll instances could
        // each register that descriptor independently. Otherwise read/write
        // consumers might race across reactors and violate socket ownership.
        FdClaim* free_claim = nullptr;
        for (auto& claim : claimed_fds_) {
            if (!claim.occupied) {
                if (!free_claim) free_claim = &claim;
                continue;
            }
            if (claim.fd == fd) return std::nullopt;
            if (transport && claim.device == identity.st_dev
                && claim.inode == identity.st_ino
                && (claim.mode & S_IFMT) == (identity.st_mode & S_IFMT)
                // Pipe read/write endpoints share one inode, but they are
                // different directional ends. Their same-direction dup
                // aliases must still be fenced. Sockets need no exception.
                && (!S_ISFIFO(identity.st_mode)
                    || claim.access_mode == access_mode))
                return std::nullopt;
        }
        // Actor terminal records may keep claims occupied after reactor slots
        // are freed. Refuse before epoll registration rather than creating an
        // untracked watch that must be cancelled after the fact.
        if (!free_claim) return std::nullopt;
        maybe_expand_locked();
        count = started_.load(std::memory_order_relaxed);
        std::size_t best = 0;
        for (std::size_t i = 1; i < count; ++i) {
            if (reactors_[i]->pending() < reactors_[best]->pending()) best = i;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t index = (best + i) % count;
            auto token = reactors_[index]->try_watch(fd, events);
            if (!token) continue;
            const Watch result{static_cast<std::uint8_t>(index), *token};
            free_claim->occupied = true;
            free_claim->fd = fd;
            free_claim->watch = result;
            free_claim->actor.reset();
            free_claim->completion_delivered = false;
            free_claim->device = identity.st_dev;
            free_claim->inode = identity.st_ino;
            free_claim->mode = identity.st_mode;
            free_claim->access_mode = access_mode;
            if (actor) free_claim->actor = *actor;
            return result;
        }
        return std::nullopt;
    }

    void abandon_actor_claims() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& claim : claimed_fds_) {
            if (!claim.occupied || !claim.actor) continue;
            claim.actor->release_resource_loan();
            claim = FdClaim{};
        }
    }
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
