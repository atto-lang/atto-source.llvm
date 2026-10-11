#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace oreslang::runtime {

/**
 * Trusted native host bridge between the existing C actor scheduler's typed
 * parent CONTROL callback and the runtime-owned supervisor process.
 *
 * NOT a guest source-level "on_panic" lowering. The host creates and owns
 * this bridge before registering its C callback. The host must drain the C
 * scheduler and the CONTROL scheduler before destroying the bridge.
 *
 * No guest policy executes from on_panic(): it only copies and admits the
 * typed event into SupervisorProcess's dedicated logical CONTROL lane.
 */
class NativeSupervisorPanicBridge final {
public:
    NativeSupervisorPanicBridge(
            OresNativeActorScheduler* scheduler,
            OresNativeActorRef parent,
            std::shared_ptr<SupervisorProcess> process)
        : scheduler_(scheduler), parent_(parent), process_(std::move(process)) {
        if (!scheduler_ || !process_ || !valid(parent_)
            || process_->runtime_id() != parent_.scheduler_domain)
            throw std::invalid_argument("supervisor bridge domain mismatch");
        if (!ores_native_actor_scheduler_poll_ready(scheduler_, parent_))
            throw std::invalid_argument("supervisor parent must be live");
    }

    /** The host must register exactly the current generation of each child. */
    void register_child(OresNativeActorRef child) {
        if (!valid(child) || child.scheduler_domain != parent_.scheduler_domain
                || child.actor_slot == parent_.actor_slot
                || !ores_native_actor_scheduler_poll_ready(scheduler_, child))
            throw std::invalid_argument("foreign, stale or stopped child");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!registered_.insert(Key{child.actor_slot, child.generation}).second)
            throw std::logic_error("child generation already registered");
    }

    /**
     * Invoked only from the C scheduler's typed CONTROL callback. Fail
     * closed on stale/foreign/replayed child events or premature delivery.
     * Return false instead of throwing through a C ABI callback boundary.
     */
    bool on_panic(std::size_t parent_slot,
                  const OresNativeActorTerminalFailure& failure) noexcept {
        try {
            if (parent_slot != parent_.actor_slot
                    || failure.kind < ORES_NATIVE_CONTROL_PANIC
                    || failure.kind > ORES_NATIVE_CONTROL_STOPPED
                    || !valid(failure.child)
                    || failure.child.scheduler_domain != parent_.scheduler_domain)
                return reject();
            Key key{failure.child.actor_slot, failure.child.generation};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (registered_.count(key) == 0) {
                    ++rejected_;
                    return false;
                }
                // Preserve finalization-before-supervisor policy ordering.
                // A native child cannot be finalized while its user turn is
                // still executing, even if stop() has been requested.
                if (!ores_native_actor_scheduler_poll_done(scheduler_, failure.child)) {
                    ++rejected_;
                    return false;
                }
                // Consume the one-shot registration before async admission.
                // An event rejected by the process must not be replayed, and
                // keeping every completed generation forever leaks memory.
                registered_.erase(key);
            }
            SupervisorFailureNotice copied{
                process_->supervisor_id(),
                process_->runtime_id(),
                failure.child.actor_slot,
                failure.child.generation,
                parent_.actor_slot,
                "native terminal failure",
                failure.kind,
                failure.failure_code,
                failure.correlation
            };
            process_->accept(copied); // enqueues only, never invokes policy inline
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++accepted_;
            }
            return true;
        } catch (...) {
            return reject(); // process counts its own failed enqueue
        }
    }

    std::size_t registered_children() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return registered_.size();
    }
    std::size_t accepted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepted_;
    }
    std::size_t rejected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

private:
    struct Key {
        std::uint64_t slot;
        std::uint64_t generation;
        bool operator==(const Key& other) const noexcept {
            return slot == other.slot && generation == other.generation;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            auto first = std::hash<std::uint64_t>{}(key.slot);
            auto second = std::hash<std::uint64_t>{}(key.generation);
            return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
        }
    };
    static bool valid(OresNativeActorRef ref) noexcept {
        return ref.scheduler_domain != 0 && ref.generation != 0;
    }
    bool reject() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejected_;
        return false;
    }

    OresNativeActorScheduler* const scheduler_; // borrowed, externally fenced
    const OresNativeActorRef parent_;
    const std::shared_ptr<SupervisorProcess> process_;
    mutable std::mutex mutex_;
    std::unordered_set<Key, KeyHash> registered_;
    std::size_t accepted_ = 0;
    std::size_t rejected_ = 0;
};

} // namespace oreslang::runtime
