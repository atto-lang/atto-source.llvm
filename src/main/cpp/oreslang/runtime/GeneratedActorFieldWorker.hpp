#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted host adapter for LLVM-generated actor field entrypoints.
 * The ActorGroup owns the lifecycle ticket; this adapter owns the heap and
 * generated field record. Source guest code never receives either pointer.
 *
 * Caller must fence pending guest continuations, callback holders and FFI
 * before polling terminal finalization. Pending native loans keep the field
 * record and heap alive, without blocking a scheduler carrier.
 */
class GeneratedActorFieldWorker final {
public:
    using Init = void* (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t);
    using Drop = void (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t, void*);

    GeneratedActorFieldWorker(NativeActorGroup& group,
                              std::uint64_t runtime_id,
                              std::uint64_t actor_id,
                              std::uint64_t incarnation,
                              std::uint64_t field_capacity,
                              Init initializer, Drop finalizer)
        : group_(group),
          actor_id_(actor_id), incarnation_(incarnation),
          stop_(std::make_shared<std::atomic<bool>>(false)),
          ref_(checked_adopt(group, runtime_id, actor_id, incarnation,
                            initializer, finalizer, stop_)),
          initializer_(initializer), finalizer_(finalizer) {
        heap_ = ores_actor_i64_heap_open(actor_id_, incarnation_, field_capacity);
    }

    GeneratedActorFieldWorker(const GeneratedActorFieldWorker&) = delete;
    GeneratedActorFieldWorker& operator=(const GeneratedActorFieldWorker&) = delete;

    ~GeneratedActorFieldWorker() {
        // A live borrowed heap cannot be silently freed. Host code must retain
        // this object until poll_finalized() acknowledges physical quiescence.
        if (heap_ != nullptr) std::terminate();
    }

    const NativeActorRef& ref() const noexcept { return ref_; }
    OresActorI64Heap* trusted_heap() const noexcept { return heap_; }

    bool initialize() {
        std::unique_lock<std::mutex> lane(lane_, std::try_to_lock);
        if (!lane.owns_lock() || initialized_ || finished_ || stop_->load())
            return false;
        bool ok = false;
        OresActorTurn turn = ores_actor_i64_heap_enter(heap_, actor_id_, incarnation_);
        try {
            record_ = initializer_(heap_, turn.domain, turn.epoch);
            if (!record_) throw std::runtime_error("generated actor initializer returned null");
            ok = true;
        } catch (...) {
            failure_ = "generated actor initialization failed";
            stop_->store(true, std::memory_order_release);
        }
        ores_actor_i64_heap_leave(heap_, turn);
        if (ok) {
            initialized_ = group_.mark_ready(ref_);
            if (!initialized_) {
                failure_ = "ActorGroup rejected generated actor readiness";
                stop_->store(true, std::memory_order_release);
            }
        }
        return initialized_;
    }

    // Executes one admitted actor turn. Carrier threads may change between
    // calls. The runtime, not the guest, supplies heap and turn credentials.
    template<class Callback, class... Args>
    auto invoke(Callback method, Args... args)
        -> decltype(method(std::declval<OresActorI64Heap*>(),
                           std::uint64_t{}, std::uint64_t{},
                           static_cast<void*>(nullptr), args...)) {
        std::unique_lock<std::mutex> lane(lane_, std::try_to_lock);
        if (!lane.owns_lock() || !initialized_ || finished_
            || stop_->load(std::memory_order_acquire))
            throw std::logic_error("generated actor turn not admitted");
        if (!method) throw std::invalid_argument("missing generated actor method");
        const OresActorTurn turn =
            ores_actor_i64_heap_enter(heap_, actor_id_, incarnation_);
        try {
            auto result = method(heap_, turn.domain, turn.epoch, record_, args...);
            ores_actor_i64_heap_leave(heap_, turn);
            return result;
        } catch (...) {
            ores_actor_i64_heap_leave(heap_, turn);
            failure_ = "generated actor handler panicked";
            stop_->store(true, std::memory_order_release);
            throw;
        }
    }

    void request_cancel() noexcept {
        stop_->store(true, std::memory_order_release);
        // Failure description is set during exclusive finalization below.
        cancelled_.store(true, std::memory_order_release);
    }

    bool stop_requested() const noexcept {
        return stop_->load(std::memory_order_acquire);
    }

    /**
     * Nonblocking physical shutdown. Must be called by the trusted lifecycle
     * executor, only after all guest continuations/wakers are fenced.
     * A native loan pins the record; no field is dropped while loaned.
     */
    bool poll_finalized() {
        std::unique_lock<std::mutex> lane(lane_, std::try_to_lock);
        if (!lane.owns_lock() || !stop_->load(std::memory_order_acquire))
            return false;
        if (finished_) return true;
        if (!ores_actor_i64_heap_quiescent(heap_)) return false;
        if (cancelled_.load(std::memory_order_acquire) && failure_.empty())
            failure_ = "actor cancelled";
        if (record_) {
            const OresActorTurn turn =
                ores_actor_i64_heap_enter(heap_, actor_id_, incarnation_);
            finalizer_(heap_, turn.domain, turn.epoch, record_);
            ores_actor_i64_heap_leave(heap_, turn);
            record_ = nullptr;
        }
        ores_actor_i64_heap_retire(heap_);
        if (ores_actor_i64_heap_try_destroy(heap_) != 1)
            throw std::logic_error("generated actor was still admitted during finalization");
        heap_ = nullptr;
        finished_ = true;
        return group_.finalized(ref_, failure_);
    }

private:
    static NativeActorRef checked_adopt(
            NativeActorGroup& group,
            std::uint64_t runtime_id, std::uint64_t actor_id,
            std::uint64_t generation, Init initializer, Drop finalizer,
            const std::shared_ptr<std::atomic<bool>>& stop) {
        if (!initializer || !finalizer)
            throw std::invalid_argument("generated actor requires init and drop");
        return group.adopt(runtime_id, actor_id, generation,
            [flag = stop] { flag->store(true, std::memory_order_release); });
    }

    NativeActorGroup& group_;
    const std::uint64_t actor_id_;
    const std::uint64_t incarnation_;
    std::shared_ptr<std::atomic<bool>> stop_;
    NativeActorRef ref_;
    Init initializer_;
    Drop finalizer_;
    OresActorI64Heap* heap_ = nullptr;
    void* record_ = nullptr;
    std::mutex lane_;
    bool initialized_ = false;
    bool finished_ = false;
    std::atomic<bool> cancelled_{false};
    std::string failure_;
};

} // namespace oreslang::runtime
