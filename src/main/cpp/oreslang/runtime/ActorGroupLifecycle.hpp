#pragma once

#include "oreslang/runtime/SupervisorFailureControl.hpp"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Native CONTROL-plane substrate only. A handle owns no actor heap and has
 * no permission to dereference actor memory; it retains a small, shared
 * readiness ticket so awaiters can observe startup failure after retirement.
 * Source-level spawn/await lowering is intentionally NOT implemented here.
 */
struct ActorReadyResult {
    bool ready = false;
    std::string error;
};

struct ActorDoneResult {
    bool success = false;
    std::string error;
};

class NativeActorRef final {
public:
    using ReadyContinuation = std::function<void(const ActorReadyResult&)>;
    using DoneContinuation = std::function<void(const ActorDoneResult&)>;

    std::uint64_t runtime_id() const noexcept { return runtime_id_; }
    std::uint64_t actor_id() const noexcept { return actor_id_; }
    std::uint64_t generation() const noexcept { return generation_; }
    /** Managed lifecycle supervisor, distinct from spawning actor. */
    std::uint64_t supervisor_id() const noexcept { return ticket_->supervisor_id; }

    // HOST-ONLY futures: guest awaits must suspend the scheduler, never block
    // a carrier using shared_future::get(). Not cancellation capabilities.
    std::shared_future<void> get_ready() const { return ticket_->ready_future; }
    std::shared_future<void> get_done() const { return ticket_->done_future; }

    /** Host supervisor can detect cancelled/dropped reserved CONTROL events. */
    bool control_delivery_lost() const noexcept {
        return ticket_->control_dropped.load(std::memory_order_acquire);
    }

    /**
     * Nonblocking host subscription to DONE. Callbacks must enqueue guest
     * resumption on the scheduler's exclusive CONTROL lane, never run guest
     * code inline from the acknowledging or subscribing host thread.
     */
    void when_done(DoneContinuation cb) const {
        if (!cb) throw std::invalid_argument("null done continuation");
        ActorDoneResult result;
        {
            std::lock_guard<std::mutex> guard(ticket_->mutex);
            if (!ticket_->done_settled) {
                ticket_->done_waiters.push_back(std::move(cb));
                return;
            }
            result = ticket_->done_result;
        }
        cb(result);
    }

    bool operator==(const NativeActorRef& other) const noexcept {
        return runtime_id_ == other.runtime_id_
            && actor_id_ == other.actor_id_
            && generation_ == other.generation_;
    }

    /**
     * Register an enqueue-only host continuation. Never run guest code from
     * this callback; the caller must post guest resumption to its scheduler.
     * May be called after actor termination without resurrecting the actor.
     */
    void when_ready(ReadyContinuation cb) const {
        if (!cb) throw std::invalid_argument("null readiness continuation");
        ActorReadyResult result;
        {
            std::lock_guard<std::mutex> guard(ticket_->mutex);
            if (!ticket_->settled) {
                ticket_->waiters.push_back(std::move(cb));
                return;
            }
            result = ticket_->result;
        }
        cb(result);
    }

private:
    struct Ticket {
        std::mutex mutex;
        bool settled = false;
        ActorReadyResult result;
        std::vector<ReadyContinuation> waiters;
        std::vector<DoneContinuation> done_waiters;
        ActorDoneResult done_result;
        std::promise<void> ready_promise;
        std::promise<void> done_promise;
        std::shared_future<void> ready_future = ready_promise.get_future().share();
        std::shared_future<void> done_future = done_promise.get_future().share();
        bool done_settled = false;
        // Stable for this exclusive managed ownership epoch. A ref retains
        // the identity through group closure/physical finalization.
        std::uint64_t supervisor_id = 0;
        std::optional<std::uint64_t> spawned_by;
        std::shared_ptr<SupervisorFailureControl> supervisor_control;
        std::atomic<bool> stop_issued{false};
        std::atomic<bool> physically_done{false};
        // True only after the group worker table was detached by close().
        std::atomic<bool> detached_from_group{false};
        std::atomic<bool> control_active{false};
        std::atomic<bool> control_reserved{false};
        std::atomic<bool> control_dropped{false};
        std::atomic<bool> logical_turn_active{false};
    };

    friend class NativeActorGroup;

    NativeActorRef(std::uint64_t runtime_id, std::uint64_t actor_id,
                   std::uint64_t generation, std::shared_ptr<Ticket> ticket)
        : runtime_id_(runtime_id), actor_id_(actor_id),
          generation_(generation), ticket_(std::move(ticket)) {}

    std::uint64_t runtime_id_;
    std::uint64_t actor_id_;
    std::uint64_t generation_;
    std::shared_ptr<Ticket> ticket_;
};

/**
 * Bounded, group-owned worker references. Membership is not ownership:
 * only explicitly adopted workers are stopped on group close. No native
 * collector or actor scheduler is embedded in this class.
 */
class NativeActorGroup final {
public:
    using ActorRef = NativeActorRef;
    using StopRequest = std::function<void()>;

    enum class ControlTurn { idle, executed, busy, retired };
    enum class UserTurn { executed, busy, retired };

    /**
     * Host scheduler ONLY: admit an ordinary actor user turn on the SAME
     * single-flight logical lane used by supervisor CONTROL. Distinct actors
     * may run concurrently on different physical carriers.
     *
     * The actor scheduler is responsible for requeueing busy turns; neither
     * admission nor resumption parks an OS thread. Reserved CONTROL takes
     * priority over new user turns. Source lowering must use this boundary.
     */
    UserTurn run_user_turn(const NativeActorRef& ref, std::function<void()> handler) {
        if (!handler) throw std::invalid_argument("user turn handler required");
        std::shared_ptr<NativeActorRef::Ticket> ticket;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* worker = find_active_locked(ref);
            if (closed_ || !worker || worker->phase == Phase::starting)
                return UserTurn::retired;
            if (worker->pending_control) return UserTurn::busy;
            ticket = worker->ref.ticket_;
            bool expected = false;
            if (!ticket->logical_turn_active.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
                return UserTurn::busy;
        }
        std::exception_ptr failure;
        try { handler(); } catch (...) { failure = std::current_exception(); }
        ticket->logical_turn_active.store(false, std::memory_order_release);
        if (failure) std::rethrow_exception(failure);
        return UserTurn::executed;
    }



    /**
     * Generation-fenced runtime retirement capability. Never holds a pointer
     * to NativeActorGroup; safe to retain after the group has been destroyed.
     * Trusted runtime only: call finalized() after physical quiescence.
     */
    class RuntimeFinalizer final {
    public:
        bool finalized(std::string failure = "") const {
            return NativeActorGroup::finalized_after_close(
                ref_, group_id_, std::move(failure));
        }
    private:
        friend class NativeActorGroup;
        RuntimeFinalizer(NativeActorRef ref, std::uint64_t group_id)
            : ref_(std::move(ref)), group_id_(group_id) {}
        NativeActorRef ref_;
        std::uint64_t group_id_;
    };

    /** Enqueue-only reserved CONTROL slot. A GPU callback posts here but
     * cannot execute actor/user handlers on its carrier thread. */
    bool enqueue_control(const NativeActorRef& ref, std::function<void()> handler) {
        if (!handler) throw std::invalid_argument("CONTROL handler required");
        std::lock_guard<std::mutex> guard(mutex_);
        Entry* worker = find_active_locked(ref);
        if (!worker || closed_ || worker->phase == Phase::starting
            || worker->pending_control) return false;
        worker->pending_control = std::move(handler);
        worker->ref.ticket_->control_reserved.store(true, std::memory_order_release);
        return true;
    }

    bool has_pending_control(const NativeActorRef& ref) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return false;
        auto it = workers_.find(ref.actor_id());
        return it != workers_.end() && it->second.ref == ref
            && bool(it->second.pending_control);
    }

    /**
     * Runtime scheduler ONLY. Single-flight group CONTROL across physical
     * carrier migrations; busy returns immediately without blocking carrier.
     * Pending tickets prevent finalization until the turn retires.
     */
    ControlTurn run_control_turn(const NativeActorRef& ref) {
        std::function<void()> handler;
        std::shared_ptr<NativeActorRef::Ticket> ticket;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* worker = find_active_locked(ref);
            if (closed_ || !worker) return ControlTurn::retired;
            if (!worker->pending_control) return ControlTurn::idle;
            ticket = worker->ref.ticket_;
            bool expected = false;
            if (!ticket->logical_turn_active.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
                return ControlTurn::busy;
            handler = std::move(worker->pending_control);
            worker->pending_control = {};
            ticket->control_reserved.store(false, std::memory_order_release);
            ticket->control_active.store(true, std::memory_order_release);
        }
        std::exception_ptr failure;
        try { handler(); } catch (...) { failure = std::current_exception(); }
        ticket->control_active.store(false, std::memory_order_release);
        ticket->logical_turn_active.store(false, std::memory_order_release);
        if (failure) std::rethrow_exception(failure);
        return ControlTurn::executed;
    }

    explicit NativeActorGroup(std::uint64_t runtime_id, std::size_t maximum_workers)
        : runtime_id_(runtime_id), maximum_workers_(maximum_workers),
          group_id_(next_group_id().fetch_add(1, std::memory_order_relaxed)),
          supervisor_control_(std::make_shared<SupervisorFailureControl>(
              group_id_, std::min<std::size_t>(maximum_workers, 128))) {
        if (runtime_id == 0 || maximum_workers == 0)
            throw std::invalid_argument("invalid ActorGroup runtime/capacity");
    }

    NativeActorGroup(const NativeActorGroup&) = delete;
    NativeActorGroup& operator=(const NativeActorGroup&) = delete;

    ~NativeActorGroup() { close(); }

    /** Host-level identity; a group does not create a supervisor actor. */
    std::uint64_t supervisor_id() const noexcept { return group_id_; }

    /**
     * Trusted host adapter supplies an enqueue-only scheduler. The callback
     * is never directly invoked from a child actor's finalized() carrier.
     */
    void install_supervisor_failure_handler(
            SupervisorFailureControl::Handler handler,
            SupervisorFailureControl::Enqueue enqueue) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("closed group cannot install supervisor handler");
        supervisor_control_->install(std::move(handler), std::move(enqueue));
    }

    std::size_t undelivered_supervisor_failures() const {
        return supervisor_control_->undelivered();
    }

    /**
     * Install one host supervisor PROCESS on the runtime's existing CONTROL
     * scheduler. The process owns its own exclusive callback lease and can
     * cooperatively suspend by returning nullopt from its policy. This is
     * NOT compiler lowering for source-level define supervisor/on_panic.
     */
    std::shared_ptr<SupervisorProcess> install_supervisor_process(
            std::size_t capacity, SupervisorProcess::Enqueue enqueue,
            SupervisorProcess::Policy policy,
            SupervisorProcess::DecisionSink decisions) {
        if (!enqueue || !policy || !decisions)
            throw std::invalid_argument("supervisor process callbacks required");
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_)
            throw std::logic_error("closed group cannot create supervisor process");
        auto process = std::make_shared<SupervisorProcess>(
            group_id_, runtime_id_, capacity, enqueue, std::move(policy),
            std::move(decisions));
        supervisor_control_->install(
            [process](const SupervisorFailureNotice& notice) {
                process->accept(notice);
            }, enqueue);
        return process;
    }

    /** Root/host adoption: exactly one exclusive lifecycle manager. */
    NativeActorRef adopt(std::uint64_t runtime_id, std::uint64_t actor_id,
                         std::uint64_t generation, StopRequest stop_request) {
        return adopt_impl(runtime_id, actor_id, generation, std::move(stop_request),
                          nullptr);
    }

    /**
     * Nested actor admission inherits its parent's SAME ActorGroup. This
     * records spawn lineage without granting actor code group-management
     * authority, implicit event membership, or a new supervisor.
     *
     * Trusted runtime/scheduler entry point only. The parent must be a live,
     * generation-checked actor in THIS group. The parent/group checks and
     * child admission are atomic with group close.
     */
    NativeActorRef adopt_child(const NativeActorRef& parent,
                               std::uint64_t runtime_id, std::uint64_t actor_id,
                               std::uint64_t generation, StopRequest stop_request) {
        return adopt_impl(runtime_id, actor_id, generation, std::move(stop_request),
                          &parent);
    }

    /** Only diagnostic spawn lineage, never a capability or supervisor ref. */
    std::optional<std::uint64_t> spawned_by(const NativeActorRef& child) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const Entry* entry = find_active_locked(child);
        if (!entry) throw std::invalid_argument("unknown or stale child actor reference");
        return entry->spawned_by;
    }

    /**
     * Capture before closing the group. This capability belongs to the
     * trusted scheduler/actor runtime, never Oreslang guest user code.
     * The ordinary group.finalized(ref) remains the primary live-group API.
     */
    RuntimeFinalizer runtime_finalizer(const NativeActorRef& ref) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = workers_.find(ref.actor_id());
        if (ref.runtime_id() != runtime_id_ || it == workers_.end()
            || !(it->second.ref == ref) || it->second.ref.ticket_ != ref.ticket_)
            throw std::invalid_argument("unknown or stale runtime finalizer");
        return RuntimeFinalizer(ref, group_id_);
    }

    /**
     * Successful startup settles the readiness ticket; await spawn yields
     * its existing handle. The native executor/continuation scheduler must
     * invoke this on successful actor initialization.
     */
    bool mark_ready(const NativeActorRef& ref) {
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* entry = find_active_locked(ref);
            if (!entry || entry->phase != Phase::starting || closed_) return false;
            entry->phase = Phase::ready;
            callbacks = settle_ticket(entry->ref, {true, ""});
        }
        notify(callbacks, ref.ticket_->result);
        return true;
    }

    /**
     * Physical terminal acknowledgment, not stop/cancel. The runtime must
     * call this ONLY after all turns, continuations, wakers, loans, heap
     * drops, finalizers and carrier references have quiesced. An old ref
     * cannot acknowledge a newer generation, even after group close.
     */
    bool finalized(const NativeActorRef& ref, std::string failure = "") {
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        std::vector<NativeActorRef::DoneContinuation> done_callbacks;
        auto& owners = registry();
        {
            std::scoped_lock guard(mutex_, owners.mutex);
            const auto it = owners.live.find(Key{ref.runtime_id(), ref.actor_id()});
            if (it == owners.live.end() || it->second.group_id != group_id_
                || it->second.generation != ref.generation()
                || it->second.ticket != ref.ticket_)
                return false;
            // Do not mutate readiness or completion state if CONTROL work
            // is still active or reserved. Failed admission is side-effect-free.
            if (ref.ticket_->logical_turn_active.load(std::memory_order_acquire)
                || ref.ticket_->control_active.load(std::memory_order_acquire)
                || ref.ticket_->control_reserved.load(std::memory_order_acquire))
                return false;
            // Close may settle readiness after detaching the worker table,
            // outside the group lock. The ticket mutex decides the winner.
            ActorReadyResult rejected{false,
                failure.empty() ? "actor terminated before ready" : failure};
            callbacks = settle_ticket(ref, std::move(rejected));
            // A cancelled or failed startup is also a terminal failure,
            // even when its physical cleanup acknowledgment has no separate
            // error argument. Inspect readiness under the ticket mutex:
            // close() may have rejected startup on another thread.
            if (failure.empty()) {
                std::lock_guard<std::mutex> ticket_guard(ref.ticket_->mutex);
                if (!ref.ticket_->result.ready)
                    failure = ref.ticket_->result.error;
            }
            // Settle done ONLY on acknowledged physical finalization.
            done_callbacks = settle_done(ref, failure);
            ref.ticket_->physically_done.store(true, std::memory_order_release);
            workers_.erase(ref.actor_id());
            owners.live.erase(it);
        }
        notify(callbacks, ref.ticket_->result);
        notify_done(done_callbacks, ref.ticket_->done_result);
        if (!failure.empty() && ref.ticket_->supervisor_control) {
            ref.ticket_->supervisor_control->publish(SupervisorFailureNotice{
                ref.supervisor_id(), ref.runtime_id(), ref.actor_id(),
                ref.generation(), ref.ticket_->spawned_by, failure
            });
        }
        return true;
    }

    // Legacy name: explicitly an already-terminated actor, never request
    // retirement. Kept for existing native host tests.
    bool retire(const NativeActorRef& ref,
                std::string error = "actor terminated before ready") {
        return finalized(ref, std::move(error));
    }

    /**
     * Request stop once for each managed actor without blocking a carrier.
     * Stage stop callbacks and notification capacity *before* changing
     * phases, so an allocation failure cannot strand a "stopping" worker
     * whose stop request was never actually dispatched.
     */
    void stop_workers() {
        struct Rejection {
            NativeActorRef ref;
            ActorReadyResult result;
            std::vector<NativeActorRef::ReadyContinuation> callbacks;
        };
        std::vector<std::pair<NativeActorRef, StopRequest>> stops;
        std::vector<Rejection> rejections;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            stops.reserve(workers_.size());
            rejections.reserve(workers_.size());
            for (const auto& item : workers_) {
                const Entry& worker = item.second;
                if (worker.phase == Phase::stopping) continue;
                stops.emplace_back(worker.ref, worker.stop_request);
                if (worker.phase == Phase::starting) {
                    // Construct every failure string BEFORE touching any
                    // readiness ticket or stop phase. OOM leaves the entire
                    // group unchanged and allows a later stop/close retry.
                    rejections.push_back(
                        Rejection{worker.ref, {false, "worker stopped before ready"}, {}});
                }
            }
            for (auto& rejection : rejections)
                rejection.callbacks = settle_ticket(
                    rejection.ref, std::move(rejection.result));
            for (auto& item : workers_) {
                Entry& worker = item.second;
                if (worker.phase != Phase::stopping)
                    worker.phase = Phase::stopping;
            }
        }
        for (auto& rejection : rejections)
            notify(rejection.callbacks, rejection.ref.ticket_->result);
        request_stops(stops);
    }

    /**
     * Close never allocates to collect stop callbacks. Detach the entire
     * worker table with unordered_map::swap under the admission lock, then
     * process every detached entry independently. An exception from a
     * readiness callback can never prevent the corresponding stop request.
     */
    void close() noexcept {
        std::unordered_map<std::uint64_t, Entry> detached;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            closed_ = true;
            detached.swap(workers_);
            // Publish only AFTER detaching group ownership. The runtime-held
            // finalizer may retire an actor following true physical quiescence.
            for (const auto& item : detached)
                item.second.ref.ticket_->detached_from_group.store(
                    true, std::memory_order_release);
        }
        for (auto& item : detached) {
            Entry& worker = item.second;
            if (worker.pending_control) {
                worker.pending_control = {};
                worker.ref.ticket_->control_reserved.store(false, std::memory_order_release);
                worker.ref.ticket_->control_dropped.store(true, std::memory_order_release);
            }
            if (worker.phase == Phase::starting) {
                try {
                    ActorReadyResult rejected{false, "ActorGroup closed before ready"};
                    auto callbacks = settle_ticket(worker.ref, std::move(rejected));
                    notify(callbacks, worker.ref.ticket_->result);
                } catch (...) {
                    // Keep processing all detached workers. A true OOM can
                    // still prevent notifying an awaiter; it must never
                    // prevent the group from requesting actor shutdown.
                }
            }
            if (worker.phase != Phase::stopping)
                dispatch_stop(worker.ref, worker.stop_request);
        }
    }

    std::size_t worker_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return workers_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }

private:
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    struct Owner {
        std::uint64_t generation;
        std::uint64_t group_id;
        std::shared_ptr<NativeActorRef::Ticket> ticket;
    };
    struct OwnerRegistry {
        std::mutex mutex;
        std::map<Key, Owner> live;
    };
    static OwnerRegistry& registry() {
        static OwnerRegistry instance;
        return instance;
    }
    static std::atomic<std::uint64_t>& next_group_id() {
        static std::atomic<std::uint64_t> next{1};
        return next;
    }
    enum class Phase { starting, ready, stopping };
    struct Entry {
        NativeActorRef ref;
        StopRequest stop_request;
        Phase phase;
        std::function<void()> pending_control;
        std::optional<std::uint64_t> spawned_by;
    };

    Entry* find_active_locked(const NativeActorRef& ref) {
        if (ref.runtime_id() != runtime_id_) return nullptr;
        const auto it = workers_.find(ref.actor_id());
        if (it == workers_.end() || !(it->second.ref == ref)
            || it->second.ref.ticket_ != ref.ticket_) return nullptr;
        return &it->second;
    }

    const Entry* find_active_locked(const NativeActorRef& ref) const {
        if (ref.runtime_id() != runtime_id_) return nullptr;
        const auto it = workers_.find(ref.actor_id());
        if (it == workers_.end() || !(it->second.ref == ref)
            || it->second.ref.ticket_ != ref.ticket_) return nullptr;
        return &it->second;
    }

    NativeActorRef adopt_impl(std::uint64_t runtime_id, std::uint64_t actor_id,
                              std::uint64_t generation, StopRequest stop_request,
                              const NativeActorRef* parent) {
        if (!stop_request || actor_id == 0 || generation == 0)
            throw std::invalid_argument("invalid native actor admission");
        if (runtime_id != runtime_id_)
            throw std::invalid_argument("foreign runtime actor reference");
        auto& owners = registry();
        // Lock order stays the original atomic group/owner pair. Never call
        // adopt() recursively with either mutex held.
        std::scoped_lock guard(mutex_, owners.mutex);
        if (closed_) throw std::logic_error("ActorGroup is closed");
        std::optional<std::uint64_t> spawned_by;
        if (parent) {
            const Entry* owner = find_active_locked(*parent);
            if (!owner || owner->phase == Phase::stopping)
                throw std::logic_error("nested spawn parent is stale, stopping, or foreign");
            spawned_by = parent->actor_id();
        }
        if (workers_.find(actor_id) != workers_.end())
            throw std::logic_error("actor already managed by group");
        if (workers_.size() >= maximum_workers_)
            throw std::logic_error("ActorGroup worker capacity exceeded");
        const Key key{runtime_id, actor_id};
        if (owners.live.find(key) != owners.live.end())
            throw std::logic_error("actor already managed by another group");
        auto ticket = std::make_shared<NativeActorRef::Ticket>();
        ticket->supervisor_id = group_id_;
        ticket->spawned_by = spawned_by;
        ticket->supervisor_control = supervisor_control_;
        NativeActorRef ref(runtime_id, actor_id, generation, std::move(ticket));
        workers_.emplace(actor_id, Entry{ref, std::move(stop_request),
                                         Phase::starting, {}, spawned_by});
        try {
            owners.live.emplace(key, Owner{generation, group_id_, ref.ticket_});
        } catch (...) {
            workers_.erase(actor_id);
            throw;
        }
        return ref;
    }

    /**
     * Same authoritative owner registry as group.finalized(), but without a
     * NativeActorGroup pointer. A token cannot retire live group workers or
     * another generation, and duplicates cannot settle DONE twice.
     */
    static bool finalized_after_close(const NativeActorRef& ref,
                                     std::uint64_t group_id,
                                     std::string failure) {
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        std::vector<NativeActorRef::DoneContinuation> done_callbacks;
        auto& owners = registry();
        {
            std::lock_guard<std::mutex> guard(owners.mutex);
            const auto it = owners.live.find(Key{ref.runtime_id(), ref.actor_id()});
            if (it == owners.live.end()
                || it->second.group_id != group_id
                || it->second.generation != ref.generation()
                || it->second.ticket != ref.ticket_
                || !ref.ticket_->detached_from_group.load(std::memory_order_acquire)
                || ref.ticket_->physically_done.load(std::memory_order_acquire)
                || ref.ticket_->logical_turn_active.load(std::memory_order_acquire)
                || ref.ticket_->control_active.load(std::memory_order_acquire)
                || ref.ticket_->control_reserved.load(std::memory_order_acquire))
                return false;
            ActorReadyResult rejected{false,
                failure.empty() ? "actor terminated before ready" : failure};
            callbacks = settle_ticket(ref, std::move(rejected));
            if (failure.empty()) {
                std::lock_guard<std::mutex> ticket_guard(ref.ticket_->mutex);
                if (!ref.ticket_->result.ready)
                    failure = ref.ticket_->result.error;
            }
            done_callbacks = settle_done(ref, failure);
            ref.ticket_->physically_done.store(true, std::memory_order_release);
            owners.live.erase(it);
        }
        notify(callbacks, ref.ticket_->result);
        notify_done(done_callbacks, ref.ticket_->done_result);
        // Ticket holds the lane even after group close/destruction.
        // A terminal failure is reported once, only after physical quiescence.
        if (!failure.empty() && ref.ticket_->supervisor_control) {
            ref.ticket_->supervisor_control->publish(SupervisorFailureNotice{
                ref.supervisor_id(), ref.runtime_id(), ref.actor_id(),
                ref.generation(), ref.ticket_->spawned_by, failure
            });
        }
        return true;
    }

    static std::vector<NativeActorRef::ReadyContinuation> settle_ticket(
            const NativeActorRef& ref, ActorReadyResult result) {
        std::lock_guard<std::mutex> guard(ref.ticket_->mutex);
        if (ref.ticket_->settled) return {};
        std::exception_ptr failure;
        if (!result.ready)
            failure = std::make_exception_ptr(std::runtime_error(result.error));
        ref.ticket_->result = std::move(result);
        if (failure) ref.ticket_->ready_promise.set_exception(failure);
        else ref.ticket_->ready_promise.set_value();
        ref.ticket_->settled = true;
        std::vector<NativeActorRef::ReadyContinuation> callbacks;
        callbacks.swap(ref.ticket_->waiters);
        return callbacks;
    }

    static std::vector<NativeActorRef::DoneContinuation> settle_done(
            const NativeActorRef& ref, const std::string& error) {
        std::lock_guard<std::mutex> guard(ref.ticket_->mutex);
        if (ref.ticket_->done_settled) return {};
        std::exception_ptr failure;
        if (!error.empty())
            failure = std::make_exception_ptr(std::runtime_error(error));
        if (failure) ref.ticket_->done_promise.set_exception(failure);
        else ref.ticket_->done_promise.set_value();
        ref.ticket_->done_result = ActorDoneResult{error.empty(), error};
        ref.ticket_->done_settled = true;
        std::vector<NativeActorRef::DoneContinuation> callbacks;
        callbacks.swap(ref.ticket_->done_waiters);
        return callbacks;
    }

    static void notify(
            const std::vector<NativeActorRef::ReadyContinuation>& callbacks,
            const ActorReadyResult& result) noexcept {
        for (const auto& callback : callbacks) {
            try { callback(result); } catch (...) {
                // Callback dispatch must not strand other waiters or workers.
            }
        }
    }

    static void notify_done(
            const std::vector<NativeActorRef::DoneContinuation>& callbacks,
            const ActorDoneResult& result) noexcept {
        for (const auto& callback : callbacks) {
            try { callback(result); } catch (...) {
                // A failed host subscription cannot strand another subscriber.
            }
        }
    }

    static void dispatch_stop(const NativeActorRef& ref,
                              const StopRequest& stop) noexcept {
        if (ref.ticket_->physically_done.load(std::memory_order_acquire)) return;
        if (ref.ticket_->stop_issued.exchange(true, std::memory_order_acq_rel))
            return;
        try { stop(); } catch (...) { /* do not strand sibling workers */ }
    }

    static void request_stops(
            const std::vector<std::pair<NativeActorRef, StopRequest>>& stops) noexcept {
        for (const auto& item : stops) dispatch_stop(item.first, item.second);
    }

    const std::uint64_t runtime_id_;
    const std::size_t maximum_workers_;
    const std::uint64_t group_id_;
    std::shared_ptr<SupervisorFailureControl> supervisor_control_;
    mutable std::mutex mutex_;
    bool closed_ = false;
    std::unordered_map<std::uint64_t, Entry> workers_;
};

}  // namespace oreslang::runtime
