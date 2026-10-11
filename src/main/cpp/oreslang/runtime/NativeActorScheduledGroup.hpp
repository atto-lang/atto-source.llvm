#pragma once

#include "oreslang/runtime/ActorExclusiveScheduler.hpp"
#include "oreslang/runtime/ActorGroupOwnedHeap.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/*
 * Trusted, scheduler-owned multi-actor admission adapter.
 *
 * One canonical NativeActorGroup owns all worker heap records; every child
 * gets a separate bounded DATA lane. One independent parent/supervisor lane
 * receives reserved CONTROL notifications. Guest and supervisor callbacks
 * execute only on their respective admitted lanes. Completion producers
 * never run parent user callbacks on their physical carrier threads.
 *
 * This is the reusable native integration boundary, not arbitrary source
 * spawn/await type checking. The surrounding host owns the carrier executor
 * and MUST keep this adapter alive until all get_done() observers have
 * settled. No second ActorGroup registry is created.
 */
class NativeActorScheduledGroup final
        : public std::enable_shared_from_this<NativeActorScheduledGroup> {
public:
    using Task = ActorExclusiveScheduler::Task;
    using Executor = ActorExclusiveScheduler::Executor;
    using Method = std::int64_t (*)(OresActorI64Heap*, std::uint64_t,
                                    std::uint64_t, OresActorI64Fields*, std::int64_t);
    using ReadyCallback = std::function<void(const ActorReadyResult&)>;
    using DoneCallback = std::function<void(const std::string&)>;
    using ResultCallback = std::function<void(std::optional<std::int64_t>,
                                              const std::string&)>;

    struct Worker final {
        const NativeActorRef ref;
        const std::shared_ptr<NativeActorHeapWorker> heap_owner;
        const std::shared_ptr<ActorExclusiveScheduler> lane;
        Worker(NativeActorRef actor_ref,
               std::shared_ptr<NativeActorHeapWorker> heap,
               std::shared_ptr<ActorExclusiveScheduler> scheduler)
            : ref(std::move(actor_ref)), heap_owner(std::move(heap)),
              lane(std::move(scheduler)) {}
    };
    using WorkerRef = std::shared_ptr<Worker>;

    static std::shared_ptr<NativeActorScheduledGroup> create(
            std::uint64_t runtime_id, std::size_t max_workers,
            std::size_t mailbox_capacity, Executor executor) {
        return std::shared_ptr<NativeActorScheduledGroup>(
            new NativeActorScheduledGroup(runtime_id, max_workers,
                                          mailbox_capacity, std::move(executor)));
    }

    NativeActorScheduledGroup(const NativeActorScheduledGroup&) = delete;
    NativeActorScheduledGroup& operator=(const NativeActorScheduledGroup&) = delete;

    WorkerRef spawn(std::uint64_t actor_id, std::uint64_t generation,
                    std::uint64_t slots, NativeActorFieldEntrypoints descriptor) {
        std::weak_ptr<NativeActorScheduledGroup> weak = shared_from_this();
        auto heap = group_.spawn(actor_id, generation, slots, descriptor,
            [weak] { if (auto owner = weak.lock()) owner->request_control(); });
        auto lane = std::make_shared<ActorExclusiveScheduler>(
            mailbox_capacity_, executor_, [weak, worker = std::weak_ptr<NativeActorHeapWorker>(heap)] {
                if (auto owned = worker.lock()) owned->request_panic();
                if (auto supervisor = weak.lock()) supervisor->request_control();
            });
        auto result = std::make_shared<Worker>(heap->ref(), heap, lane);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workers_.push_back(result);
        }
        // Initialization is never invoked on the calling/guest carrier.
        if (!lane->post_data([heap] { (void)heap->initialize(); },
                             [heap] { heap->request_cancel(); })) {
            heap->request_cancel();
        }
        return result;
    }

    // Resolves on the parent's own exclusive CONTROL lane, even if
    // readiness was already settled when the observer registered.
    void on_ready(const WorkerRef& child, ReadyCallback callback) {
        require_member(child);
        if (!callback) throw std::invalid_argument("empty actor ready callback");
        std::weak_ptr<NativeActorScheduledGroup> weak = shared_from_this();
        child->ref.when_ready([weak, callback = std::move(callback)](const ActorReadyResult& ready) {
            if (auto owner = weak.lock()) {
                owner->push_control_event([callback, ready] { callback(ready); });
            }
        });
    }

    // Future observer is retained until *physical* terminal ack; neither
    // cancellation nor close can synthesize a successful get_done().
    void on_done(const WorkerRef& child, DoneCallback callback) {
        require_member(child);
        if (!callback) throw std::invalid_argument("empty actor done callback");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_observers_.push_back({child, std::move(callback)});
        }
        request_control();
    }

    // Return false on mailbox capacity or actor shutdown. In either case
    // caller still receives exactly one asynchronous failure completion.
    bool invoke_i64(const WorkerRef& child, Method generated,
                    std::int64_t argument, ResultCallback callback) {
        require_member(child);
        if (!generated || !callback)
            throw std::invalid_argument("null generated actor method or result continuation");
        std::weak_ptr<NativeActorScheduledGroup> weak = shared_from_this();
        auto cb = std::make_shared<ResultCallback>(std::move(callback));
        auto settled = std::make_shared<std::atomic<bool>>(false);
        auto finish = [weak, cb, settled](std::optional<std::int64_t> result,
                                         std::string error) {
            if (settled->exchange(true, std::memory_order_acq_rel)) return;
            if (auto owner = weak.lock()) {
                owner->push_control_event([cb, result, error] {
                    (*cb)(result, error);
                });
            }
        };
        const bool accepted = child->lane->post_data(
            [heap = child->heap_owner, generated, argument, finish] {
                std::int64_t result = 0;
                const bool ok = heap->with_turn([&](auto* h, auto turn, auto* fields) {
                    result = generated(h, turn.domain, turn.epoch, fields, argument);
                });
                if (ok) finish(result, "");
                else finish(std::nullopt, "actor failed or stopped");
            },
            [finish] { finish(std::nullopt, "actor call cancelled before admission"); });
        if (!accepted) finish(std::nullopt, "actor mailbox closed or full");
        return accepted;
    }

    void stop(const WorkerRef& child) {
        require_member(child);
        child->lane->close();
        child->heap_owner->request_normal_stop();
        request_control();
    }

    void cancel(const WorkerRef& child) {
        require_member(child);
        child->lane->close();
        child->heap_owner->request_cancel();
        request_control();
    }

    void close() {
        std::vector<WorkerRef> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = workers_;
        }
        for (const auto& child : snapshot) {
            child->lane->close();
            child->heap_owner->request_cancel();
        }
        group_.close();
        request_control();
    }

    // Parent CTRL handlers are scheduled from edge-triggered loan release,
    // worker terminal request, readiness and completion changes. Running
    // children never block the parent's lane waiting for their own mutex.
    std::size_t finalized_count() const noexcept {
        return finalized_.load(std::memory_order_acquire);
    }

private:
    struct DoneObserver {
        WorkerRef child;
        DoneCallback callback;
    };

    NativeActorScheduledGroup(std::uint64_t runtime_id, std::size_t capacity,
                              std::size_t mailbox_capacity, Executor executor)
        : group_(runtime_id, capacity), mailbox_capacity_(mailbox_capacity),
          executor_(std::move(executor)),
          parent_lane_(std::make_shared<ActorExclusiveScheduler>(
              capacity + 1, executor_)) {
        if (!mailbox_capacity || !executor_)
            throw std::invalid_argument("invalid scheduled ActorGroup executor/capacity");
    }

    void require_member(const WorkerRef& child) const {
        if (!child) throw std::invalid_argument("null actor reference");
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (const auto& current : workers_) {
            if (current == child) { found = true; break; }
        }
        if (!found) throw std::invalid_argument("foreign actor group worker");
    }

    void push_control_event(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(task));
        }
        request_control();
    }

    void request_control() {
        std::weak_ptr<NativeActorScheduledGroup> weak = shared_from_this();
        // CONTROL is independent of bounded DATA even during close.
        (void)parent_lane_->post_control([weak] {
            if (auto owner = weak.lock()) owner->process_control();
        });
    }

    void process_control() {
        // Never block parent CONTROL behind a running child turn.
        finalized_.fetch_add(group_.drain_control(), std::memory_order_acq_rel);
        std::deque<Task> pending;
        std::vector<DoneObserver> terminal;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(events_);
            auto iter = done_observers_.begin();
            while (iter != done_observers_.end()) {
                if (iter->child->ref.get_done().wait_for(std::chrono::seconds(0))
                        == std::future_status::ready) {
                    terminal.push_back(std::move(*iter));
                    iter = done_observers_.erase(iter);
                } else {
                    ++iter;
                }
            }
        }
        for (auto& callback : pending) {
            try { callback(); } catch (...) { /* never strand sibling CONTROL */ }
        }
        for (const auto& observer : terminal) {
            std::string error;
            try { observer.child->ref.get_done().get(); }
            catch (const std::exception& ex) { error = ex.what(); }
            try { observer.callback(error); }
            catch (...) { /* do not prevent other terminal observers */ }
        }
    }

    NativeActorOwnedHeapGroup group_;
    const std::size_t mailbox_capacity_;
    const Executor executor_;
    const std::shared_ptr<ActorExclusiveScheduler> parent_lane_;
    mutable std::mutex mutex_;
    std::vector<WorkerRef> workers_;
    std::deque<Task> events_;
    std::vector<DoneObserver> done_observers_;
    std::atomic<std::size_t> finalized_{0};
};

} // namespace oreslang::runtime
