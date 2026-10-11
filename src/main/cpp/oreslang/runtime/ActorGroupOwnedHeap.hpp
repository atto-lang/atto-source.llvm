#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/*
 * Trusted native source-actor heap owner, attached to the ONE canonical
 * NativeActorGroup registry. Not a second ActorGroup and not a thread pool.
 *
 * The real scheduler is responsible for invoking process_control() on the
 * parent's exclusive CONTROL lane after the enqueue-only wakeup. It must
 * fence all callback holders before attempting final physical destruction.
 *
 * A stop/panic cannot drop a borrowed field. Instead it immediately rejects
 * new source turns, waits for loans/FFI callbacks to release, and only then
 * enters a dedicated exclusive cleanup turn, drops the field record,
 * retires the heap, physically destroys it, and acknowledges get_done().
 */
struct NativeActorFieldEntrypoints {
    using Init = OresActorI64Fields* (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t);
    using Drop = void (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                          OresActorI64Fields*);
    using LayoutHash = std::uint64_t (*)();
    using LayoutVersion = std::uint32_t (*)();
    Init init = nullptr;
    Drop drop = nullptr;
    LayoutHash layout_hash = nullptr;
    LayoutVersion layout_version = nullptr;
    std::uint32_t field_count = 0;
    std::uint64_t expected_layout_hash = 0;
};

enum class NativeActorExit : int {
    running = 0,
    normal = 1,
    cancelled = 2,
    panicked = 3
};

class NativeActorHeapWorker final : public std::enable_shared_from_this<NativeActorHeapWorker> {
public:
    using EnqueueControl = std::function<void()>;
    using TurnBody = std::function<void(OresActorI64Heap*, OresActorTurn,
                                        OresActorI64Fields*)>;

    // A source-await/FFI completion may retain a continuation without
    // borrowing any concrete actor field. Pin the host dispatch token, not
    // the native heap pointer. This is independent of slot loan accounting.
    class ContinuationPin final {
    public:
        ContinuationPin(const ContinuationPin&) = delete;
        ContinuationPin& operator=(const ContinuationPin&) = delete;
        ContinuationPin(ContinuationPin&& other) noexcept
            : owner_(std::move(other.owner_)) {}
        ContinuationPin& operator=(ContinuationPin&& other) noexcept {
            if (this != &other) { release(); owner_ = std::move(other.owner_); }
            return *this;
        }
        ~ContinuationPin() { release(); }
        void release() noexcept {
            if (owner_) {
                auto retained = std::move(owner_);
                retained->release_continuation_pin();
            }
        }
    private:
        friend class NativeActorHeapWorker;
        explicit ContinuationPin(std::shared_ptr<NativeActorHeapWorker> owner)
            : owner_(std::move(owner)) {}
        std::shared_ptr<NativeActorHeapWorker> owner_;
    };

    NativeActorHeapWorker(NativeActorGroup& group,
                          std::uint64_t actor_id, std::uint64_t generation,
                          std::uint64_t slot_capacity,
                          NativeActorFieldEntrypoints generated,
                          EnqueueControl enqueue_control)
        : group_(group), actor_id_(actor_id), generation_(generation),
          slot_capacity_(slot_capacity), generated_(generated),
          enqueue_control_(std::move(enqueue_control)) {
        if (!actor_id_ || !generation_ || !slot_capacity_ || slot_capacity_ > 4096
            || !generated_.init || !generated_.drop || !enqueue_control_
            || !generated_.layout_hash || !generated_.layout_version
            || !generated_.field_count || slot_capacity_ < generated_.field_count)
            throw std::invalid_argument("invalid generated native actor field descriptor");
        if (generated_.layout_version() != 1
                || generated_.layout_hash() != generated_.expected_layout_hash)
            throw std::invalid_argument("actor field layout version/hash mismatch");
    }

    NativeActorHeapWorker(const NativeActorHeapWorker&) = delete;
    NativeActorHeapWorker& operator=(const NativeActorHeapWorker&) = delete;

    void attach(NativeActorRef ref) {
        std::lock_guard<std::mutex> guard(lane_);
        if (ref_ || ref.actor_id() != actor_id_ || ref.generation() != generation_)
            throw std::logic_error("invalid or duplicate native actor adoption");
        ref_.emplace(std::move(ref));
    }

    NativeActorRef ref() const {
        std::lock_guard<std::mutex> guard(lane_);
        if (!ref_) throw std::logic_error("actor was not adopted");
        return *ref_;
    }

    /*
     * Initialization barrier. The native heap and record are created by the
     * owner, under the first admitted exclusive turn, never by guest source.
     * Constructor failures that are C aborts cannot be caught by this layer.
     */
    bool initialize() {
        bool ready = false;
        {
            std::lock_guard<std::mutex> guard(lane_);
            if (!ref_ || heap_ || finalized_) throw std::logic_error("actor initialized twice");
            if (requested_.load(std::memory_order_acquire) != NativeActorExit::running)
                return false;
            heap_ = ores_actor_i64_heap_open(actor_id_, generation_, slot_capacity_);
            const OresActorTurn turn =
                ores_actor_i64_heap_enter(heap_, actor_id_, generation_);
            try {
                fields_ = generated_.init(heap_, turn.domain, turn.epoch);
                if (!fields_) throw std::runtime_error("actor initializer returned null fields");
                initialized_ = true;
                ready = true;
            } catch (...) {
                failure_ = "actor initializer panicked";
                requested_.store(NativeActorExit::panicked, std::memory_order_release);
            }
            ores_actor_i64_heap_leave(heap_, turn);
        }
        if (ready) {
            // Readiness callbacks are never invoked with the actor lane held.
            (void) group_.mark_ready(ref());
        }
        if (requested_.load(std::memory_order_acquire) != NativeActorExit::running)
            enqueue_control_();
        return ready;
    }

    /*
     * Real generated method calls go through this exclusive logical lane.
     * C++ exceptions are caught at the host boundary and converted into
     * parent CONTROL delivery; no guest method is called from a callback.
     */
    bool with_turn(const TurnBody& body) {
        if (!body) throw std::invalid_argument("null actor turn body");
        bool ok = true;
        {
            std::lock_guard<std::mutex> guard(lane_);
            if (!heap_ || !fields_ || finalized_ || !initialized_
                || requested_.load(std::memory_order_acquire) != NativeActorExit::running)
                return false;
            const OresActorTurn turn =
                ores_actor_i64_heap_enter(heap_, actor_id_, generation_);
            try {
                body(heap_, turn, fields_);
            } catch (...) {
                failure_ = "actor handler panicked";
                requested_.store(NativeActorExit::panicked, std::memory_order_release);
                ok = false;
            }
            ores_actor_i64_heap_leave(heap_, turn);
        }
        if (!ok || requested_.load(std::memory_order_acquire) != NativeActorExit::running)
            enqueue_control_();
        return ok;
    }

    // Stop/panic are enqueue-only. No native field finalizer or parent
    // on_panic() runs on the terminating child's carrier thread.
    void request_normal_stop() { request(NativeActorExit::normal); }
    void request_cancel() { request(NativeActorExit::cancelled); }

    void request_panic() {
        {
            std::lock_guard<std::mutex> guard(lane_);
            failure_ = "actor panicked";
            requested_.store(NativeActorExit::panicked, std::memory_order_release);
        }
        enqueue_control_();
    }

    // Create only while an actor is running and admitted; the returned
    // move-only pin must live until callback/waker/continuation release.
    ContinuationPin pin_continuation() {
        // Independent host-only atomic pin avoids trying to reacquire the
        // actor lane from inside its own executing method/await lowering.
        if (!initialized_.load(std::memory_order_acquire)
            || finalized_.load(std::memory_order_acquire)
            || requested_.load(std::memory_order_acquire) != NativeActorExit::running)
            throw std::logic_error("continuation admission after actor stop");
        std::uint32_t current = continuation_pins_.load(std::memory_order_acquire);
        do {
            if (current == UINT32_MAX)
                throw std::overflow_error("too many pinned actor continuations");
        } while (!continuation_pins_.compare_exchange_weak(
            current, current + 1, std::memory_order_acq_rel));
        if (requested_.load(std::memory_order_acquire) != NativeActorExit::running
            || finalized_.load(std::memory_order_acquire)) {
            continuation_pins_.fetch_sub(1, std::memory_order_acq_rel);
            throw std::logic_error("actor stopped while pinning continuation");
        }
        return ContinuationPin(shared_from_this());
    }

    std::uint32_t pending_continuations() const {
        return continuation_pins_.load(std::memory_order_acquire);
    }

    // A suspended loan is held by the future/FFI operation. Releasing it
    // may happen on a foreign carrier, but cannot itself finalize the actor.
    void release_field_loan(OresActorLoan loan) {
        bool stopped = false;
        {
            std::lock_guard<std::mutex> guard(lane_);
            if (!heap_ || finalized_) throw std::logic_error("loan after physical retirement");
            ores_actor_i64_heap_release(heap_, loan);
            stopped = requested_.load(std::memory_order_acquire) != NativeActorExit::running;
        }
        if (stopped) enqueue_control_();
    }

    /* Called by parent/supervisor CONTROL admission only. Returns false
     * if the actor cannot yet finalize (active pin or no terminal request).
     * Finalization is physical: get_done never completes before real free. */
    bool process_control() {
        std::optional<NativeActorRef> finalized_ref;
        std::string error;
        {
            // CONTROL runs on the parent's lane. Never park that carrier
            // behind an in-flight child turn; its completion re-enqueues.
            std::unique_lock<std::mutex> guard(lane_, std::try_to_lock);
            if (!guard.owns_lock()) return false;
            const NativeActorExit reason = requested_.load(std::memory_order_acquire);
            if (reason == NativeActorExit::running || finalized_) return false;
            if (heap_) {
                if (continuation_pins_.load(std::memory_order_acquire) != 0
                    || ores_actor_i64_heap_loan_count(heap_) != 0) return false;
                if (fields_) {
                    const OresActorTurn turn =
                        ores_actor_i64_heap_enter(heap_, actor_id_, generation_);
                    generated_.drop(heap_, turn.domain, turn.epoch, fields_);
                    fields_ = nullptr;
                    ores_actor_i64_heap_leave(heap_, turn);
                }
                ores_actor_i64_heap_retire(heap_);
                if (ores_actor_i64_heap_try_destroy(heap_) != 1)
                    throw std::logic_error("actor heap unexpectedly not quiescent");
                heap_ = nullptr;
            }
            finalized_ = true;
            finalized_ref = ref_;
            error = reason == NativeActorExit::normal ? ""
                  : reason == NativeActorExit::cancelled ? "actor cancelled"
                  : failure_.empty() ? "actor panicked" : failure_;
        }
        // Resolve ready/done only after real heap retirement, outside lane
        // lock so notifications may safely enqueue further supervisor work.
        if (!finalized_ref || !group_.finalized(*finalized_ref, error))
            throw std::logic_error("native group rejected physical finalization");
        return true;
    }

    bool physically_finalized() const {
        std::lock_guard<std::mutex> guard(lane_);
        return finalized_;
    }

    std::uint32_t pending_loans() const {
        std::lock_guard<std::mutex> guard(lane_);
        return heap_ ? ores_actor_i64_heap_loan_count(heap_) : 0;
    }

private:
    void release_continuation_pin() noexcept {
        // A continuation may complete inline while its actor's logical lane
        // is still held. Reacquiring lane_ here self-deadlocks the scheduler.
        // The pin count and stop reason are independent atomics.
        if (continuation_pins_.fetch_sub(1, std::memory_order_acq_rel) == 0)
            std::terminate();
        if (requested_.load(std::memory_order_acquire) != NativeActorExit::running) {
            try { enqueue_control_(); }
            catch (...) { /* runtime must supply a reliable CONTROL enqueue */ }
        }
    }

    void request(NativeActorExit desired) {
        NativeActorExit expected = NativeActorExit::running;
        if (requested_.compare_exchange_strong(
                expected, desired, std::memory_order_acq_rel))
            enqueue_control_();
    }

    NativeActorGroup& group_;
    const std::uint64_t actor_id_;
    const std::uint64_t generation_;
    const std::uint64_t slot_capacity_;
    const NativeActorFieldEntrypoints generated_;
    const EnqueueControl enqueue_control_;
    mutable std::mutex lane_;
    std::optional<NativeActorRef> ref_;
    OresActorI64Heap* heap_ = nullptr;
    OresActorI64Fields* fields_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> finalized_{false};
    std::string failure_;
    std::atomic<std::uint32_t> continuation_pins_{0};
    std::atomic<NativeActorExit> requested_{NativeActorExit::running};
};

class NativeActorOwnedHeapGroup final {
public:
    NativeActorOwnedHeapGroup(std::uint64_t runtime_id, std::size_t maximum_workers)
        : group_(runtime_id, maximum_workers), runtime_id_(runtime_id) {}

    NativeActorOwnedHeapGroup(const NativeActorOwnedHeapGroup&) = delete;
    NativeActorOwnedHeapGroup& operator=(const NativeActorOwnedHeapGroup&) = delete;

    std::shared_ptr<NativeActorHeapWorker> spawn(
        std::uint64_t actor_id, std::uint64_t generation,
        std::uint64_t slot_capacity, NativeActorFieldEntrypoints generated,
        NativeActorHeapWorker::EnqueueControl enqueue_control) {
        auto worker = std::make_shared<NativeActorHeapWorker>(
            group_, actor_id, generation, slot_capacity, generated,
            std::move(enqueue_control));
        std::weak_ptr<NativeActorHeapWorker> weak = worker;
        auto ref = group_.adopt(runtime_id_, actor_id, generation,
                                [weak] { if (auto owner = weak.lock()) owner->request_cancel(); });
        worker->attach(ref);
        workers_.push_back(worker);
        return worker;
    }

    void request_stop_all() { group_.stop_workers(); }
    void close() { group_.close(); }

    // Caller must invoke on the parent CONTROL lane. A pending loan or
    // continuation prevents success, rather than fabricating get_done().
    std::size_t drain_control() {
        std::size_t finalized = 0;
        for (const auto& worker : workers_)
            if (worker->process_control()) ++finalized;
        return finalized;
    }

    NativeActorGroup& lifecycle() noexcept { return group_; }

private:
    NativeActorGroup group_;
    std::uint64_t runtime_id_;
    std::vector<std::shared_ptr<NativeActorHeapWorker>> workers_;
};

}  // namespace oreslang::runtime
