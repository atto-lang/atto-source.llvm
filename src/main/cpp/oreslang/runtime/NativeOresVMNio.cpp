#include "oreslang/runtime/NativeOresVMNio.h"
#include "oreslang/runtime/NativeOresVMActorNio.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>

namespace {
constexpr std::size_t kClaimCapacity = 256;

static bool same_actor(OresNativeActorRef left, OresNativeActorRef right) noexcept {
    return left.scheduler_domain != 0
        && left.scheduler_domain == right.scheduler_domain
        && left.actor_slot == right.actor_slot
        && left.generation == right.generation;
}

static bool same_watch(OresNativeNioWatch left, OresNativeNioWatch right) noexcept {
    return left.ticket == right.ticket && left.reactor == right.reactor;
}

static bool same_completion(
        const OresNativeNioCompletion& left,
        const OresNativeNioCompletion& right) noexcept {
    return same_watch(left.watch, right.watch)
        && left.fd == right.fd
        && left.events == right.events
        && left.outcome == right.outcome;
}

/*
 * A VM actor carrier may use only the actor-owned bridge and must name the
 * exact actor allocation capability currently executing on that carrier.
 * Trusted outer-host runtime code has no current VM execution and may operate
 * on behalf of an exact ActorRef.
 */
static bool caller_matches_actor(
        OresNativeVM* vm,
        enum OresNativeVMDomain domain,
        OresNativeActorRef actor) noexcept {
    if (!vm || domain < ORES_NATIVE_VM_MAIN
            || domain >= ORES_NATIVE_VM_DOMAIN_COUNT
            || !actor.scheduler_domain || !actor.generation)
        return false;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        OresNativeExecutionContext current{};
        if (!ores_native_vm_current_execution_context(
                vm, static_cast<enum OresNativeVMDomain>(i), &current))
            continue;
        return i == static_cast<int>(domain)
            && current.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR
            && current.allocation.domain == actor.scheduler_domain
            && current.allocation.owner == actor.actor_slot
            && current.allocation.generation == actor.generation;
    }
    return true;
}

static bool on_vm_actor_carrier(OresNativeVM* vm) noexcept {
    if (!vm) return false;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        OresNativeExecutionContext current{};
        if (ores_native_vm_current_execution_context(
                vm, static_cast<enum OresNativeVMDomain>(i), &current))
            return true;
    }
    return false;
}

enum class ClaimState : std::uint8_t {
    free = 0,
    watching = 1,
    pending = 2,
    delivered = 3
};

struct ActorClaim {
    ClaimState state = ClaimState::free;
    std::uint64_t generation = 0; // sticky across reuse; UINT64_MAX retires slot
    OresNativeNioWatch watch{};
    enum OresNativeVMDomain domain = ORES_NATIVE_VM_MAIN;
    OresNativeActorRef actor{};
    int fd = -1;
    std::uint32_t events = 0;
    OresNativeNioCompletion completion{};
    bool await_bound = false;
    OresNativeWakeTicket wake{};
};

struct HostPending {
    bool occupied = false;
    OresNativeNioCompletion completion{};
};

class TryGate final {
public:
    explicit TryGate(std::mutex& gate) noexcept {
        try {
            lock_ = std::unique_lock<std::mutex>(gate, std::try_to_lock);
        } catch (...) {
            // C ABI admission is fail-closed; no C++ exception crosses it.
        }
    }
    bool owns_lock() const noexcept { return lock_.owns_lock(); }
private:
    std::unique_lock<std::mutex> lock_{};
};

struct VmNioBackend {
    OresNativeVM* vm = nullptr;
    OresNativeNio* raw = nullptr;
    std::mutex gate;
    std::array<ActorClaim, kClaimCapacity> claims{};
    std::array<HostPending, kClaimCapacity> host_pending{};

    VmNioBackend(OresNativeVM* owner, OresNativeNio* reactor)
        : vm(owner), raw(reactor) {}

    bool fd_reserved_locked(int fd) const noexcept {
        for (const auto& claim : claims)
            if (claim.state != ClaimState::free && claim.fd == fd)
                return true;
        for (const auto& pending : host_pending)
            if (pending.occupied && pending.completion.fd == fd)
                return true;
        return false;
    }

    ActorClaim* find_claim_locked(OresNativeNioWatch watch) noexcept {
        for (auto& claim : claims)
            if (claim.state != ClaimState::free
                    && same_watch(claim.watch, watch))
                return &claim;
        return nullptr;
    }

    ActorClaim* match_claim_locked(
            const OresNativeActorNioWatch& token) noexcept {
        if (token.claim_slot == 0 || token.claim_slot > claims.size())
            return nullptr;
        auto& claim = claims[token.claim_slot - 1];
        return claim.state != ClaimState::free
                && claim.generation == token.claim_generation
                && claim.domain == token.domain
                && same_actor(claim.actor, token.actor)
                && same_watch(claim.watch, token.watch)
            ? &claim : nullptr;
    }

    ActorClaim* reserve_claim_locked(
            enum OresNativeVMDomain domain,
            OresNativeActorRef actor,
            int fd,
            std::uint32_t events,
            const OresNativeWakeTicket* wake,
            OresNativeActorNioWatch* output) noexcept {
        for (std::size_t i = 0; i < claims.size(); ++i) {
            auto& claim = claims[i];
            if (claim.state != ClaimState::free
                    || claim.generation == UINT64_MAX)
                continue;
            ++claim.generation;
            if (!claim.generation) std::abort();
            claim.state = ClaimState::watching;
            claim.domain = domain;
            claim.actor = actor;
            claim.fd = fd;
            claim.events = events;
            claim.watch = {};
            claim.completion = {};
            claim.await_bound = wake != nullptr;
            claim.wake = wake ? *wake : OresNativeWakeTicket{};
            *output = OresNativeActorNioWatch{
                {}, domain, actor, static_cast<std::uint32_t>(i + 1),
                claim.generation};
            return &claim;
        }
        return nullptr;
    }

    static void free_claim_locked(ActorClaim& claim) noexcept {
        const auto generation = claim.generation;
        claim = ActorClaim{};
        claim.generation = generation;
    }

    bool stash_host_locked(const OresNativeNioCompletion& completion) noexcept {
        for (auto& pending : host_pending) {
            if (pending.occupied) continue;
            pending.occupied = true;
            pending.completion = completion;
            return true;
        }
        return false;
    }

    bool pop_stashed_host_locked(OresNativeNioCompletion* output) noexcept {
        for (auto& pending : host_pending) {
            if (!pending.occupied) continue;
            *output = pending.completion;
            pending = HostPending{};
            return true;
        }
        return false;
    }

    bool mark_actor_completion_locked(
            const OresNativeNioCompletion& completion) noexcept {
        ActorClaim* claim = find_claim_locked(completion.watch);
        if (!claim) return false;
        if (claim->state != ClaimState::watching
                || claim->fd != completion.fd)
            std::abort(); // duplicate/mismatched kernel terminal record
        claim->completion = completion;
        claim->state = ClaimState::pending;
        return true;
    }

    bool deliver_pending_actor_locked(
            OresNativeActorNioCompletion* output) noexcept {
        for (std::size_t i = 0; i < claims.size(); ++i) {
            auto& claim = claims[i];
            if (claim.state != ClaimState::pending) continue;
            claim.state = ClaimState::delivered;
            output->io = claim.completion;
            output->owner = OresNativeActorNioWatch{
                claim.watch, claim.domain, claim.actor,
                static_cast<std::uint32_t>(i + 1), claim.generation};
            return true;
        }
        return false;
    }

    bool has_free_host_pending_locked() const noexcept {
        for (const auto& pending : host_pending)
            if (!pending.occupied) return true;
        return false;
    }

    bool try_pop_host_locked(OresNativeNioCompletion* output) noexcept {
        if (pop_stashed_host_locked(output)) return true;
        for (std::size_t i = 0; i < kClaimCapacity; ++i) {
            OresNativeNioCompletion completion{};
            if (!raw || !ores_native_nio_try_pop(raw, &completion))
                return false;
            if (mark_actor_completion_locked(completion))
                continue;
            *output = completion;
            return true;
        }
        return false;
    }

    bool try_pop_actor_locked(OresNativeActorNioCompletion* output) noexcept {
        if (deliver_pending_actor_locked(output)) return true;
        for (std::size_t i = 0; i < kClaimCapacity; ++i) {
            // A host completion may be ahead of the desired actor record.
            // Never consume it unless bounded storage exists to preserve it.
            if (!has_free_host_pending_locked()) return false;
            OresNativeNioCompletion completion{};
            if (!raw || !ores_native_nio_try_pop(raw, &completion))
                return false;
            if (mark_actor_completion_locked(completion))
                return deliver_pending_actor_locked(output);
            if (!stash_host_locked(completion)) std::abort();
        }
        return false;
    }

    void retire_and_release() noexcept {
        /*
         * VM close has fenced new backend leases before this callback. Join
         * every reactor first; only then can outstanding actor claims be
         * abandoned because no later kernel completion can materialize.
         */
        OresNativeNio* reactor = nullptr;
        std::array<enum OresNativeVMDomain, kClaimCapacity> domains{};
        std::array<OresNativeActorRef, kClaimCapacity> actors{};
        std::array<OresNativeWakeTicket, kClaimCapacity> wakes{};
        std::array<bool, kClaimCapacity> has_wake{};
        std::size_t release_count = 0;
        {
            std::lock_guard<std::mutex> lock(gate);
            reactor = raw;
            raw = nullptr;
        }
        if (reactor) ores_native_nio_close(reactor);
        {
            std::lock_guard<std::mutex> lock(gate);
            for (auto& claim : claims) {
                if (claim.state == ClaimState::free) continue;
                if (release_count >= actors.size()) std::abort();
                domains[release_count] = claim.domain;
                actors[release_count] = claim.actor;
                has_wake[release_count] = claim.await_bound;
                wakes[release_count] = claim.wake;
                ++release_count;
                free_claim_locked(claim);
            }
            for (auto& pending : host_pending) pending = HostPending{};
        }
        for (std::size_t i = 0; i < release_count; ++i) {
            if (has_wake[i]) {
                const int cancelled = ores_native_vm_actor_cancel_wake(
                    vm, domains[i], wakes[i]);
                const int aborted = cancelled ? 0
                    : ores_native_vm_actor_abort_transferred_wake(
                        vm, domains[i], wakes[i]);
                if (!cancelled && !aborted) std::abort();
            }
            if (!ores_native_vm_actor_resource_release(
                    vm, domains[i], actors[i]))
                std::abort();
        }
    }
};

class NioBackendLease final {
public:
    explicit NioBackendLease(OresNativeVM* vm)
        : vm_(vm),
          ptr_(static_cast<VmNioBackend*>(ores_native_vm_nio_acquire(vm))) {}
    ~NioBackendLease() {
        if (ptr_) ores_native_vm_nio_release(vm_, ptr_);
    }
    NioBackendLease(const NioBackendLease&) = delete;
    NioBackendLease& operator=(const NioBackendLease&) = delete;
    VmNioBackend* get() const noexcept { return ptr_; }
private:
    OresNativeVM* vm_;
    VmNioBackend* ptr_;
};
}

// The callback is owned by a C-compiled VM, so use C language linkage.
extern "C" void ores_native_vm_nio_destroy_backend(void* backend) {
    auto* state = static_cast<VmNioBackend*>(backend);
    if (!state) return;
    state->retire_and_release();
    delete state;
}

extern "C" int ores_native_vm_enable_nio(
    OresNativeVM* vm, size_t max_reactors,
    int preallocated_realtime, size_t rt_reactors) {
    if (!vm || !ores_native_vm_nio_begin_bootstrap(vm)) return 0;
    OresNativeNio* raw =
        ores_native_nio_open(max_reactors, preallocated_realtime, rt_reactors);
    if (!raw) {
        ores_native_vm_nio_cancel_bootstrap(vm);
        return 0;
    }
    VmNioBackend* backend = nullptr;
    try {
        backend = new (std::nothrow) VmNioBackend(vm, raw);
    } catch (...) {
        backend = nullptr;
    }
    if (!backend) {
        ores_native_nio_close(raw);
        ores_native_vm_nio_cancel_bootstrap(vm);
        return 0;
    }
    if (!ores_native_vm_attach_nio(
            vm, backend, &ores_native_vm_nio_destroy_backend)) {
        // attach_nio consumes this thread's bootstrap reservation.
        backend->retire_and_release();
        delete backend;
        return 0;
    }
    return 1;
}

extern "C" size_t ores_native_vm_nio_started(OresNativeVM* vm) {
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    return gate.owns_lock() && backend->raw
        ? ores_native_nio_started_reactors(backend->raw) : 0;
}

extern "C" int ores_native_vm_nio_try_watch(
    OresNativeVM* vm, int fd, uint32_t events, OresNativeNioWatch* output) {
    if (!output || on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw || backend->fd_reserved_locked(fd))
        return 0;
    return ores_native_nio_try_watch(backend->raw, fd, events, output);
}

extern "C" int ores_native_vm_nio_try_watch_actor(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    int fd, uint32_t events, OresNativeActorNioWatch* output) {
    if (!output || !caller_matches_actor(vm, domain, actor)) return 0;
    *output = OresNativeActorNioWatch{};
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;

    // Retain first: this is the atomic STOP-vs-physical-admission boundary.
    if (!ores_native_vm_actor_resource_retain(vm, domain, actor)) return 0;

    bool refund = true;
    {
        TryGate gate(backend->gate);
        if (gate.owns_lock() && backend->raw
                && !backend->fd_reserved_locked(fd)) {
            OresNativeActorNioWatch candidate{};
            ActorClaim* claim = backend->reserve_claim_locked(
                domain, actor, fd, events, nullptr, &candidate);
            if (claim) {
                OresNativeNioWatch watch{};
                if (ores_native_nio_try_watch(
                        backend->raw, fd, events, &watch)) {
                    claim->watch = watch;
                    candidate.watch = watch;
                    *output = candidate;
                    refund = false;
                } else {
                    VmNioBackend::free_claim_locked(*claim);
                }
            }
        }
    }
    if (refund) {
        if (!ores_native_vm_actor_resource_release(vm, domain, actor))
            std::abort();
        return 0;
    }
    return 1;
}

extern "C" int ores_native_vm_nio_try_watch_actor_await(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    int fd, uint32_t events,
    uint64_t continuation_slot, uint64_t continuation_generation,
    uint64_t code_generation, uint64_t descriptor_generation,
    OresNativeActorNioAwait* output) {
    if (!output || !caller_matches_actor(vm, domain, actor)) return 0;
    *output = OresNativeActorNioAwait{};

    // Reserve the scheduler wake loan before the kernel operation can suspend.
    // This means STOP can never observe a gap between NIO ownership and the
    // continuation that will eventually resume/fail on the actor lane.
    OresNativeWakeTicket wake{};
    if (!ores_native_vm_actor_reserve_wake(
            vm, domain, actor, actor,
            continuation_slot, continuation_generation,
            code_generation, descriptor_generation, &wake))
        return 0;

    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) {
        if (!ores_native_vm_actor_cancel_wake(vm, domain, wake))
            std::abort();
        return 0;
    }

    // Retain second. If STOP won after wake reservation, the wake is still in
    // RESERVED state and can be refunded without admitting an epoll watch.
    if (!ores_native_vm_actor_resource_retain(vm, domain, actor)) {
        if (!ores_native_vm_actor_cancel_wake(vm, domain, wake))
            std::abort();
        return 0;
    }

    bool refund = true;
    {
        TryGate gate(backend->gate);
        if (gate.owns_lock() && backend->raw
                && !backend->fd_reserved_locked(fd)) {
            OresNativeActorNioWatch candidate{};
            ActorClaim* claim = backend->reserve_claim_locked(
                domain, actor, fd, events, &wake, &candidate);
            if (claim) {
                OresNativeNioWatch watch{};
                if (ores_native_nio_try_watch(
                        backend->raw, fd, events, &watch)) {
                    claim->watch = watch;
                    candidate.watch = watch;
                    output->watch = candidate;
                    output->wake = wake;
                    refund = false;
                } else {
                    VmNioBackend::free_claim_locked(*claim);
                }
            }
        }
    }
    if (refund) {
        if (!ores_native_vm_actor_cancel_wake(vm, domain, wake))
            std::abort();
        if (!ores_native_vm_actor_resource_release(vm, domain, actor))
            std::abort();
        return 0;
    }
    return 1;
}

extern "C" int ores_native_vm_nio_try_cancel(
    OresNativeVM* vm, OresNativeNioWatch watch) {
    if (on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw
            || backend->find_claim_locked(watch))
        return 0;
    return ores_native_nio_try_cancel(backend->raw, watch);
}

extern "C" int ores_native_vm_nio_try_cancel_actor(
    OresNativeVM* vm, OresNativeActorNioWatch watch) {
    if (!caller_matches_actor(vm, watch.domain, watch.actor)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return 0;
    ActorClaim* claim = backend->match_claim_locked(watch);
    if (!claim || claim->state != ClaimState::watching) return 0;
    return ores_native_nio_try_cancel(backend->raw, claim->watch);
}

extern "C" int ores_native_vm_nio_try_pop(
    OresNativeVM* vm, OresNativeNioCompletion* output) {
    if (!output || on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    return gate.owns_lock() && backend->raw
        && backend->try_pop_host_locked(output);
}

extern "C" int ores_native_vm_nio_try_pop_actor(
    OresNativeVM* vm, OresNativeActorNioCompletion* output) {
    if (!output || on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    return gate.owns_lock() && backend->raw
        && backend->try_pop_actor_locked(output);
}

extern "C" int ores_native_vm_nio_ack_actor_completion(
    OresNativeVM* vm, OresNativeActorNioCompletion completion) {
    if (on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    if (!gate.owns_lock()) return 0;
    ActorClaim* claim = backend->match_claim_locked(completion.owner);
    if (!claim || claim->state != ClaimState::delivered
            || claim->await_bound
            || !same_completion(claim->completion, completion.io))
        return 0;

    // The resource loan is still live, so the exact actor incarnation cannot
    // have completed/recycled. Keep the claim gate held across release: no
    // opposite lock order nests scheduler->backend anywhere in this bridge.
    if (!ores_native_vm_actor_resource_release(
            vm, claim->domain, claim->actor))
        return 0;
    VmNioBackend::free_claim_locked(*claim);
    return 1;
}

extern "C" OresNativeActorNioAwaitDispatchStatus
ores_native_vm_nio_dispatch_actor_await(
    OresNativeVM* vm, OresNativeActorNioCompletion completion) {
    if (on_vm_actor_carrier(vm)) return ORES_NATIVE_NIO_AWAIT_INVALID;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return ORES_NATIVE_NIO_AWAIT_INVALID;

    OresNativeWakeTicket wake{};
    enum OresNativeVMDomain domain = ORES_NATIVE_VM_MAIN;
    OresNativeActorRef actor{};
    OresNativeWakeOutcome outcome = ORES_NATIVE_WAKE_FAILED;

    {
        TryGate gate(backend->gate);
        if (!gate.owns_lock()) return ORES_NATIVE_NIO_AWAIT_INVALID;

        ActorClaim* claim = backend->match_claim_locked(completion.owner);
        if (!claim || claim->state != ClaimState::delivered
                || !claim->await_bound
                || !same_completion(claim->completion, completion.io))
            return ORES_NATIVE_NIO_AWAIT_INVALID;

        switch (completion.io.outcome) {
            case ORES_NATIVE_NIO_READY:
                outcome = ORES_NATIVE_WAKE_READY;
                break;
            case ORES_NATIVE_NIO_CANCELLED:
                outcome = ORES_NATIVE_WAKE_CANCELLED;
                break;
            case ORES_NATIVE_NIO_STOPPED:
                outcome = ORES_NATIVE_WAKE_FAILED;
                break;
            default:
                std::abort();
        }

        wake = claim->wake;
        domain = claim->domain;
        actor = claim->actor;

        /*
         * TRANSFER is the irreversible physical-ownership handoff. If actor
         * STOP won first, transfer rejects while the wake remains RESERVED:
         * cancel it and release the NIO loan without scheduling guest code.
         */
        if (!ores_native_vm_actor_transfer_wake(vm, domain, wake)) {
            if (!ores_native_vm_actor_cancel_wake(vm, domain, wake))
                std::abort();
            if (!ores_native_vm_actor_resource_release(vm, domain, actor))
                std::abort();
            VmNioBackend::free_claim_locked(*claim);
            return ORES_NATIVE_NIO_AWAIT_PRODUCER_RETIRED;
        }

        /*
         * A TRANSFERRED wake already blocks physical DONE and cannot lose its
         * reserved execution credit. It is therefore safe—and necessary for
         * same-FD continuation rearm—to release the NIO resource/FD claim now,
         * before the wake becomes runnable. No actor callback can execute from
         * transfer_wake itself.
         */
        if (!ores_native_vm_actor_resource_release(vm, domain, actor))
            std::abort();
        VmNioBackend::free_claim_locked(*claim);
    }

    /*
     * Publish only after dropping the NIO claim mutex. The actor wake can run
     * immediately after this call makes the lane runnable; at that point the
     * prior one-shot FD claim is already gone and a continuation may rearm it.
     */
    const OresNativeWakePublishStatus published =
        ores_native_vm_actor_publish_wake(vm, domain, wake, outcome);
    if (published == ORES_NATIVE_WAKE_INVALID) {
        // TRANSFERRED ownership is still live on INVALID. Roll it back so an
        // internal invariant failure cannot pin actor DONE indefinitely.
        if (!ores_native_vm_actor_abort_transferred_wake(vm, domain, wake))
            std::abort();
        return ORES_NATIVE_NIO_AWAIT_INVALID;
    }
    return published == ORES_NATIVE_WAKE_PUBLISHED
        ? ORES_NATIVE_NIO_AWAIT_PUBLISHED
        : ORES_NATIVE_NIO_AWAIT_PRODUCER_RETIRED;
}

extern "C" size_t ores_native_vm_nio_try_pop_batch(
    OresNativeVM* vm, OresNativeNioCompletion* output, size_t capacity) {
    if (!output || capacity == 0 || on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return 0;
    constexpr size_t max_batch = kClaimCapacity;
    const size_t limit = capacity < max_batch ? capacity : max_batch;
    size_t count = 0;
    while (count < limit && backend->try_pop_host_locked(&output[count]))
        ++count;
    return count;
}

extern "C" int ores_native_vm_nio_completion_fd(
    OresNativeVM* vm, size_t reactor) {
    if (on_vm_actor_carrier(vm)) return -1;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return -1;
    TryGate gate(backend->gate);
    return gate.owns_lock() && backend->raw
        ? ores_native_nio_completion_fd(backend->raw, reactor) : -1;
}

extern "C" void ores_native_vm_nio_drain_signal(
    OresNativeVM* vm, size_t reactor) {
    if (on_vm_actor_carrier(vm)) return;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return;
    TryGate gate(backend->gate);
    if (gate.owns_lock() && backend->raw)
        ores_native_nio_drain_completion_signal(backend->raw, reactor);
}

namespace oreslang::runtime {
// Keep the exact existing VM backend lease across each host operation. An
// accepted watch owns a persistent callback permit in the backend ledger
// until its terminal completion is explicitly acknowledged.
bool vm_nio_actor_try_watch(OresNativeVM* vm,
        NativeTransportCallbackGate& owner, int fd, std::uint32_t events,
        OresNativeNioWatch* output) noexcept {
    if (!vm || !output) return false;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return false;
    // Same non-parking gate and FD reservation fence as the host path.
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw || backend->fd_reserved_locked(fd))
        return false;
    return nio_actor_try_watch(backend->raw, owner, fd, events, output);
}
bool vm_nio_actor_try_cancel(OresNativeVM* vm,
                             OresNativeNioWatch watch) noexcept {
    if (!vm) return false;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return false;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return false;
    return nio_actor_try_cancel(backend->raw, watch);
}
bool vm_nio_actor_stop(OresNativeVM* vm) noexcept {
    if (!vm) return false;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return false;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return false;
    return nio_actor_stop(backend->raw);
}
bool vm_nio_actor_try_pop(OresNativeVM* vm,
                          ActorNioCompletion* output) noexcept {
    if (!vm || !output) return false;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return false;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return false;
    return nio_actor_try_pop(backend->raw, output);
}
bool vm_nio_actor_acknowledge(OresNativeVM* vm,
                              const ActorNioCompletion& completion) noexcept {
    if (!vm) return false;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return false;
    TryGate gate(backend->gate);
    if (!gate.owns_lock() || !backend->raw) return false;
    return nio_actor_acknowledge(backend->raw, completion);
}
} // namespace oreslang::runtime
