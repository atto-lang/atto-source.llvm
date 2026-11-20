#pragma once

#include "oreslang/runtime/GpuActorHostTurnBridgeV0.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::gpu_actor {

/*
 * G1 host-only provider port. An event is minted by the trusted provider
 * instance, scoped to its exact actor/tenant/device/queue/generations/turn.
 * A source actor never supplies a completion Boolean or constructs an Event.
 *
 * The native provider remains trusted. Minting/observing this in-memory handle
 * alone is NOT physical event authentication; a real OpenCL/other backend
 * must bind it to its native command and verify actual device quiescence.
 */
class ProviderPortV1 {
public:
    using Turn = HostTurnBridgeV0::Turn;
    struct Binding {
        std::string actor_id;
        std::uint64_t actor_generation;
        std::string tenant;
        std::string device;
        std::string queue;
        std::uint64_t device_generation;
        std::uint64_t queue_generation;
        std::uint64_t allocation_generation;

        bool operator==(const Binding& b) const noexcept {
            return actor_id == b.actor_id && actor_generation == b.actor_generation
                && tenant == b.tenant && device == b.device && queue == b.queue
                && device_generation == b.device_generation
                && queue_generation == b.queue_generation
                && allocation_generation == b.allocation_generation;
        }
    };

    class Event final {
        friend class ProviderPortV1;
        Event(Binding binding, Turn turn, std::uint64_t serial,
              std::shared_ptr<const std::uint8_t> proof)
            : binding_(std::move(binding)), turn_(turn), serial_(serial),
              proof_(std::move(proof)) {}
        Binding binding_;
        Turn turn_;
        std::uint64_t serial_;
        std::shared_ptr<const std::uint8_t> proof_;
    public:
        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;
        std::uint64_t serial() const noexcept { return serial_; }
    };
    using EventHandle = std::shared_ptr<const Event>;

    enum class EnqueueState { Submitted, DefinitelyNotSubmitted, Unknown };
    struct EnqueueResult {
        EnqueueState state;
        EventHandle event;
    };
    // Complete means the trusted native backend has verified *the matching*
    // event as quiescent. DeviceLost/Unknown do not imply quiescence.
    enum class Observation { Pending, CompleteOk, CompleteFailed, DeviceLost, Unknown };

    explicit ProviderPortV1(Binding binding) : binding_(std::move(binding)) {
        if (binding_.actor_id.empty() || binding_.actor_generation == 0
                || binding_.tenant.empty() || binding_.device.empty()
                || binding_.queue.empty() || binding_.device_generation == 0
                || binding_.queue_generation == 0
                || binding_.allocation_generation == 0) {
            throw std::invalid_argument("GPU_PROVIDER_INVALID_BINDING");
        }
    }
    ProviderPortV1(const ProviderPortV1&) = delete;
    ProviderPortV1& operator=(const ProviderPortV1&) = delete;
    virtual ~ProviderPortV1() = default;

    const Binding& binding() const noexcept { return binding_; }
    bool authentic_local_event(const EventHandle& event, Turn turn) const noexcept {
        return event && event->proof_ == seal_
            && event->serial_ != 0
            && event->turn_.actor_generation == turn.actor_generation
            && event->turn_.sequence == turn.sequence
            && event->binding_ == binding_;
    }

    // Calls MUST have bounded duration/nonblocking behavior at scheduler edge.
    // Unknown is mandatory if the backend cannot prove no enqueue occurred.
    virtual EnqueueResult enqueue(Turn turn) = 0;
    virtual EnqueueResult reconcile_unknown(Turn turn) = 0;
    virtual Observation observe(const EventHandle& event) = 0;
    // True requires physical buffer release, not mere host-handle dropping.
    virtual bool try_release_buffer() = 0;

protected:
    EventHandle issue_event(Turn turn) {
        if (turn.actor_generation != binding_.actor_generation || turn.sequence == 0
                || serial_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::logic_error("GPU_PROVIDER_ISSUE_EVENT_INVALID");
        }
        return EventHandle(new Event(binding_, turn, ++serial_, seal_));
    }

private:
    Binding binding_;
    const std::shared_ptr<const std::uint8_t> seal_ =
        std::make_shared<const std::uint8_t>(0);
    std::uint64_t serial_ = 0;
};

/*
 * Supervisor-owned, bounded retention registry. The actor-facing wrapper can
 * be destroyed while a command is in flight: the registry still owns the
 * native host-turn ledger, provider port, and pre-reserved CONTROL lane.
 *
 * This registry MUST outlive unresolved provider operations. A higher-level
 * supervisor must retain it through restart/reconciliation. No capacity
 * overcommit, timed "success", implicit CPU fallback or speculative release.
 */
class GpuActorQuarantineRegistryV1 final
    : public std::enable_shared_from_this<GpuActorQuarantineRegistryV1> {
public:
    using Turn = HostTurnBridgeV0::Turn;
    using ActorKey = HostTurnBridgeV0::ActorKey;
    using Port = ProviderPortV1;
    using EventHandle = Port::EventHandle;

private:
    enum class Phase { Ready, Retiring, EnqueueInProgress, UnknownEnqueue, Reconciling, Submitted, Terminal, Releasing, Released };

    struct Entry final {
        explicit Entry(std::shared_ptr<Port> p,
                       std::shared_ptr<oreslang::SupervisorFailureLane> c)
            : port(std::move(p)), control(std::move(c)) {}
        std::mutex mu;
        std::shared_ptr<Port> port;
        std::shared_ptr<oreslang::SupervisorFailureLane> control;
        std::unique_ptr<HostTurnBridgeV0> host;
        std::optional<Turn> turn;
        EventHandle event;
        Phase phase = Phase::Ready;
        bool stopping = false;
        bool detached = false;
        bool device_lost = false;

        void stop_locked(bool device_loss) {
            if (phase == Phase::Released) return;
            if (device_loss) device_lost = true;
            if (!stopping) {
                stopping = true;
                if (device_loss) (void)host->report_device_loss();
                else (void)host->request_close();
            }
            if (host->closed() && !turn && phase != Phase::Retiring
                    && phase != Phase::Releasing) phase = Phase::Terminal;
        }

        void accept_result_locked(const Port::EnqueueResult& result) {
            if (!turn || (phase != Phase::EnqueueInProgress &&
                           phase != Phase::UnknownEnqueue)) {
                throw std::logic_error("GPU_PROVIDER_UNEXPECTED_SUBMISSION");
            }
            if (result.state == Port::EnqueueState::Submitted) {
                if (!port->authentic_local_event(result.event, *turn)) {
                    // Could still have submitted physical work. Never abort.
                    stop_locked(true);
                    phase = Phase::UnknownEnqueue;
                    throw std::logic_error("GPU_PROVIDER_FOREIGN_OR_STALE_EVENT");
                }
                host->mark_submitted(*turn);
                event = result.event;
                phase = Phase::Submitted;
            } else if (result.state == Port::EnqueueState::DefinitelyNotSubmitted) {
                if (result.event) throw std::logic_error("GPU_PROVIDER_SPURIOUS_EVENT");
                host->abort_enqueue(*turn);
                event.reset();
                turn.reset();
                phase = Phase::Terminal;
            } else {
                if (result.event) throw std::logic_error("GPU_PROVIDER_UNKNOWN_HAS_EVENT");
                phase = Phase::UnknownEnqueue;
            }
        }

        Turn submit(std::int64_t delta) {
            std::unique_lock<std::mutex> guard(mu);
            if (phase != Phase::Ready || stopping || detached)
                throw std::logic_error("GPU_PROVIDER_ACTOR_NOT_READY");
            const Turn next = host->prepare(delta);
            host->begin_enqueue(next);
            turn = next;
            phase = Phase::EnqueueInProgress;
            guard.unlock();

            Port::EnqueueResult result{Port::EnqueueState::Unknown, nullptr};
            try {
                result = port->enqueue(next);
            } catch (...) {
                // An exception may have occurred after the physical enqueue.
                guard.lock();
                stop_locked(true);
                phase = Phase::UnknownEnqueue;
                throw;
            }

            guard.lock();
            try {
                accept_result_locked(result);
            } catch (...) {
                // A malformed provider reply is NOT proof that no command
                // reached the device. Preserve the enqueue window and keep
                // the actor under supervisor quarantine for reconciliation.
                if (phase == Phase::EnqueueInProgress) {
                    stop_locked(true);
                    phase = Phase::UnknownEnqueue;
                }
                throw;
            }
            return next;
        }

        void progress() {
            std::unique_lock<std::mutex> guard(mu);
            if (phase == Phase::EnqueueInProgress || phase == Phase::Released
                    || phase == Phase::Ready) return;
            if (phase == Phase::UnknownEnqueue) {
                const Turn current = *turn;
                phase = Phase::Reconciling; // one provider reconciliation at a time
                guard.unlock();
                Port::EnqueueResult result{Port::EnqueueState::Unknown, nullptr};
                try { result = port->reconcile_unknown(current); }
                catch (...) {
                    guard.lock();
                    if (phase == Phase::Reconciling) phase = Phase::UnknownEnqueue;
                    return; // ambiguous outcome is NOT an abort
                }
                guard.lock();
                if (phase == Phase::Reconciling) {
                    phase = Phase::UnknownEnqueue;
                    try { accept_result_locked(result); }
                    catch (const std::logic_error&) {
                        // Malformed reconciliation from a trusted-native
                        // provider is a host failure, not ordinary polling.
                        // Keep the ambiguous device loan quarantined.
                        stop_locked(true);
                    }
                }
                return;
            }
            if (phase == Phase::Submitted) {
                const Turn current = *turn;
                const auto submitted_event = event;
                guard.unlock();
                Port::Observation observed = Port::Observation::Unknown;
                try { observed = port->observe(submitted_event); }
                catch (...) { return; }
                guard.lock();
                if (phase != Phase::Submitted || event != submitted_event) return;
                if (!port->authentic_local_event(event, current)) {
                    stop_locked(true);
                    return;
                }
                if (observed == Port::Observation::DeviceLost) {
                    stop_locked(true);
                    return; // loss is NOT a quiescence certificate
                }
                if (observed == Port::Observation::Pending ||
                        observed == Port::Observation::Unknown) return;
                if (stopping) {
                    host->acknowledge_cancelled_provider_event(current, true);
                } else {
                    host->finish_after_provider_event(
                        current, observed == Port::Observation::CompleteOk, true);
                }
                turn.reset();
                event.reset();
                phase = host->closed() ? Phase::Terminal : Phase::Ready;
                return;
            }
            if (phase == Phase::Terminal) {
                // Reserve the external release attempt before dropping the
                // mutex. Concurrent progress/recovery must never call the
                // provider release operation twice on the same allocation.
                phase = Phase::Releasing;
                guard.unlock();
                bool released = false;
                try { released = port->try_release_buffer(); }
                catch (...) {
                    guard.lock();
                    if (phase == Phase::Releasing) phase = Phase::Terminal;
                    return;
                }
                guard.lock();
                if (phase != Phase::Releasing) return;
                if (!released) {
                    phase = Phase::Terminal;
                    return;
                }
                host->acknowledge_backend_buffer_release(true);
                phase = Phase::Released;
            }
        }

        void retire_gracefully() {
            std::unique_lock<std::mutex> guard(mu);
            if (phase != Phase::Ready || detached || stopping || turn)
                throw std::logic_error("GPU_PROVIDER_GRACEFUL_RETIRE_DENIED");
            // Reserve retirement before releasing the mutex for the provider
            // call. No competing thread may admit a new turn on the buffer.
            phase = Phase::Retiring;
            guard.unlock();
            bool released = false;
            try {
                released = port->try_release_buffer();
            } catch (...) {
                guard.lock();
                if (phase == Phase::Retiring)
                    phase = stopping ? Phase::Terminal : Phase::Ready;
                throw;
            }
            guard.lock();
            if (phase != Phase::Retiring)
                throw std::logic_error("GPU_PROVIDER_RETIRE_STATE_CORRUPT");
            if (stopping) {
                // request_close/device_loss can win while native release is
                // in progress. Reuse the existing outcome, never call the
                // provider release a second time. CONTROL was pre-reserved.
                if (released) {
                    host->acknowledge_backend_buffer_release(true);
                    phase = Phase::Released;
                } else {
                    phase = Phase::Terminal;
                }
                throw std::logic_error("GPU_PROVIDER_RELEASE_NOT_PROVEN");
            }
            if (!released) {
                phase = Phase::Ready;
                throw std::logic_error("GPU_PROVIDER_RELEASE_NOT_PROVEN");
            }
            host->retire_gracefully_after_backend_release(true);
            phase = Phase::Released;
        }
    };

public:
    explicit GpuActorQuarantineRegistryV1(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0 || capacity > 65536)
            throw std::invalid_argument("GPU_PROVIDER_INVALID_REGISTRY_CAPACITY");
    }
    GpuActorQuarantineRegistryV1(const GpuActorQuarantineRegistryV1&) = delete;
    GpuActorQuarantineRegistryV1& operator=(const GpuActorQuarantineRegistryV1&) = delete;

    std::uint64_t admit(std::shared_ptr<Port> provider,
                        std::shared_ptr<oreslang::SupervisorFailureLane> lane,
                        ActorKey actor, std::string parent, std::uint64_t bytes,
                        std::int64_t initial_state = 0) {
        if (!provider || !lane || provider->binding().actor_id != actor.id
                || provider->binding().actor_generation != actor.generation)
            throw std::invalid_argument("GPU_PROVIDER_ADMISSION_BINDING_MISMATCH");

        std::lock_guard<std::mutex> guard(mu_);
        if (entries_.size() >= capacity_)
            throw std::logic_error("GPU_PROVIDER_QUARANTINE_CAPACITY_EXHAUSTED");
        if (next_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("GPU_PROVIDER_REGISTRY_SEQUENCE_EXHAUSTED");

        // Reserve the bounded slot BEFORE pinning the process root.
        // An allocation failure in map::emplace must not leave an empty
        // supervisor registry permanently rooted.
        const auto id = next_id_ + 1;
        auto [it, inserted] = entries_.emplace(id, nullptr);
        if (!inserted) throw std::logic_error("GPU_PROVIDER_REGISTRY_COLLISION");
        try {
            // Process-root retention is required before any actor CONTROL
            // reservation or physical resource can become live.
            if (entries_.size() == 1) process_root().pin(shared_from_this());
            auto e = std::make_shared<Entry>(std::move(provider), std::move(lane));
            const auto& b = e->port->binding();
            e->host = std::make_unique<HostTurnBridgeV0>(
                std::move(actor), std::move(parent), *e->control,
                oreslang::gpu::Identity{b.tenant, b.device, b.allocation_generation},
                bytes, initial_state);
            it->second = std::move(e);
            next_id_ = id;
        } catch (...) {
            entries_.erase(it);
            if (entries_.empty()) process_root().unpin(this);
            throw;
        }
        return id;
    }

    Turn submit(std::uint64_t id, std::int64_t delta) { return require(id)->submit(delta); }
    void request_close(std::uint64_t id) {
        auto e = require(id);
        std::lock_guard<std::mutex> guard(e->mu);
        e->stop_locked(false);
    }
    void report_device_loss(std::string tenant, std::string device,
                            std::uint64_t device_generation) {
        for (auto& e : snapshot()) {
            const auto& b = e->port->binding();
            if (b.tenant == tenant && b.device == device &&
                    b.device_generation == device_generation) {
                std::lock_guard<std::mutex> guard(e->mu);
                e->stop_locked(true);
            }
        }
    }

    // Actor wrapper teardown only detaches the actor. The registry retains
    // the provider, CONTROL lane, and unresolved native ledger until release.
    void detach(std::uint64_t id) noexcept {
        try {
            auto e = require(id);
            {
                std::lock_guard<std::mutex> guard(e->mu);
                e->detached = true;
                e->stop_locked(false);
            }
            prune(id, e);
        } catch (...) {
            // No destructor-time exception and, crucially, no physical release.
        }
    }

    void progress_all() {
        for (auto& [id, e] : snapshot_with_ids()) {
            e->progress();
            prune(id, e);
        }
    }

    void retire_gracefully(std::uint64_t id) {
        auto e = require(id);
        e->retire_gracefully();
        prune(id, e);
    }

    // The root supervisor (not a guest actor) can recover all registries
    // retained after their immediate supervisor handle was destroyed.
    static void progress_process_root() {
        for (const auto& r : process_root().snapshot()) r->progress_all();
    }
    static std::size_t retained_registry_count() {
        return process_root().count();
    }

    std::size_t retained_count() const {
        std::lock_guard<std::mutex> guard(mu_);
        return entries_.size();
    }
    std::size_t quarantined_count() const {
        std::size_t n = 0;
        for (const auto& e : snapshot()) {
            std::lock_guard<std::mutex> guard(e->mu);
            if (e->detached && e->phase != Phase::Released) ++n;
        }
        return n;
    }
    std::int64_t committed_state(std::uint64_t id) const {
        auto e = require(id);
        return e->host->committed_state();
    }
    bool completed(std::uint64_t id) const {
        auto e = require(id);
        std::lock_guard<std::mutex> guard(e->mu);
        return e->phase == Phase::Released;
    }

private:
    using EntryPtr = std::shared_ptr<Entry>;

    struct ProcessRoot final {
        std::mutex mu;
        std::map<GpuActorQuarantineRegistryV1*,
                 std::shared_ptr<GpuActorQuarantineRegistryV1>> retained;
        static constexpr std::size_t maximum_registries = 256;

        void pin(const std::shared_ptr<GpuActorQuarantineRegistryV1>& registry) {
            std::lock_guard<std::mutex> guard(mu);
            if (retained.count(registry.get()) != 0) return;
            if (retained.size() >= maximum_registries)
                throw std::logic_error("GPU_PROVIDER_PROCESS_ROOT_CAPACITY_EXHAUSTED");
            retained.emplace(registry.get(), registry);
        }
        void unpin(GpuActorQuarantineRegistryV1* registry) {
            std::lock_guard<std::mutex> guard(mu);
            retained.erase(registry);
        }
        std::vector<std::shared_ptr<GpuActorQuarantineRegistryV1>> snapshot() {
            std::lock_guard<std::mutex> guard(mu);
            std::vector<std::shared_ptr<GpuActorQuarantineRegistryV1>> out;
            out.reserve(retained.size());
            for (const auto& kv : retained) out.push_back(kv.second);
            return out;
        }
        std::size_t count() {
            std::lock_guard<std::mutex> guard(mu);
            return retained.size();
        }
    };
    static ProcessRoot& process_root() {
        // This root intentionally survives native supervisor-instance teardown.
        // Process termination is NOT crash-persistent hardware recovery.
        static auto* root = new ProcessRoot();
        return *root;
    }
    EntryPtr require(std::uint64_t id) const {
        std::lock_guard<std::mutex> guard(mu_);
        const auto it = entries_.find(id);
        if (it == entries_.end() || !it->second)
            throw std::logic_error("GPU_PROVIDER_UNKNOWN_ACTOR_LEASE");
        return it->second;
    }
    std::vector<EntryPtr> snapshot() const {
        std::lock_guard<std::mutex> guard(mu_);
        std::vector<EntryPtr> result;
        result.reserve(entries_.size());
        for (const auto& [_, e] : entries_) result.push_back(e);
        return result;
    }
    std::vector<std::pair<std::uint64_t, EntryPtr>> snapshot_with_ids() const {
        std::lock_guard<std::mutex> guard(mu_);
        return std::vector<std::pair<std::uint64_t, EntryPtr>>(entries_.begin(), entries_.end());
    }
    void prune(std::uint64_t id, const EntryPtr& e) {
        std::lock_guard<std::mutex> guard(mu_);
        std::lock_guard<std::mutex> entry_guard(e->mu);
        if (e->detached && e->phase == Phase::Released)
            entries_.erase(id);
        if (entries_.empty()) process_root().unpin(this);
    }
    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::map<std::uint64_t, EntryPtr> entries_;
    std::uint64_t next_id_ = 0;
};

/*
 * Guest-facing actor ownership is mediated through a scoped native lease;
 * user code can never inject an Event or claim completion via a Boolean.
 */
class GpuActorProviderBridgeV1 final {
public:
    using Registry = GpuActorQuarantineRegistryV1;
    GpuActorProviderBridgeV1(std::shared_ptr<Registry> registry,
                             std::shared_ptr<ProviderPortV1> port,
                             std::shared_ptr<oreslang::SupervisorFailureLane> control,
                             Registry::ActorKey actor,
                             std::string parent, std::uint64_t buffer_bytes,
                             std::int64_t initial_state = 0)
        : registry_(std::move(registry)) {
        if (!registry_) throw std::invalid_argument("GPU_PROVIDER_MISSING_REGISTRY");
        id_ = registry_->admit(std::move(port), std::move(control),
                               std::move(actor), std::move(parent),
                               buffer_bytes, initial_state);
    }
    GpuActorProviderBridgeV1(const GpuActorProviderBridgeV1&) = delete;
    GpuActorProviderBridgeV1& operator=(const GpuActorProviderBridgeV1&) = delete;
    ~GpuActorProviderBridgeV1() { if (registry_) registry_->detach(id_); }

    HostTurnBridgeV0::Turn submit(std::int64_t delta) { return registry_->submit(id_,delta); }
    void request_close() { registry_->request_close(id_); }
    void retire_gracefully() { registry_->retire_gracefully(id_); }
    std::int64_t committed_state() const { return registry_->committed_state(id_); }
    bool completed() const { return registry_->completed(id_); }

private:
    std::shared_ptr<Registry> registry_;
    std::uint64_t id_ = 0;
};

} // namespace oreslang::gpu_actor
