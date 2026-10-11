#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"

#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted host bookkeeping for logical child identity, distinct from the
 * current native actor (slot, generation). No guest authority or restart
 * implementation is conferred by this registry.
 *
 * The actor lifecycle owner MUST prove physical finalization before marking
 * a generation finalized and MUST have admitted a live replacement before
 * calling replace(). Neither operation is performed here.
 */
class SupervisorChildRegistry final {
public:
    struct ChildRef {
        std::uint64_t supervisor_id;
        std::uint64_t runtime_id;
        std::uint64_t logical_child_id;
        bool operator==(const ChildRef& rhs) const noexcept {
            return supervisor_id == rhs.supervisor_id
                && runtime_id == rhs.runtime_id
                && logical_child_id == rhs.logical_child_id;
        }
    };
    enum class Phase { active, finalized, stopped };
    struct Snapshot {
        ChildRef child;
        OresNativeActorRef actor;
        std::uint64_t incarnation;
        Phase phase;
    };

    SupervisorChildRegistry(std::uint64_t supervisor_id, std::uint64_t runtime_id)
        : supervisor_id_(supervisor_id), runtime_id_(runtime_id) {
        if (!supervisor_id_ || !runtime_id_)
            throw std::invalid_argument("invalid supervisor child registry");
    }

    Snapshot register_child(OresNativeActorRef actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        validate_actor(actor);
        if (active_actors_.count(actor_key(actor)))
            throw std::logic_error("native actor already bound to a logical child");
        if (next_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("logical child ID exhausted");
        ChildRef child{supervisor_id_, runtime_id_, ++next_id_};
        Snapshot first{child, actor, 1, Phase::active};
        children_.emplace(child.logical_child_id, first);
        active_actors_.emplace(actor_key(actor), child.logical_child_id);
        return first;
    }

    Snapshot snapshot(ChildRef child) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return require_current(child);
    }

    Snapshot finalized(ChildRef child, OresNativeActorRef actor,
                       std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto current = require_expected(
            child, actor, incarnation, Phase::active);
        active_actors_.erase(actor_key(current.actor));
        auto& stored = children_.at(child.logical_child_id);
        stored.phase = Phase::finalized;
        return stored;
    }

    /** Identity commit only: caller must already have admitted replacement. */
    Snapshot replace(ChildRef child, OresNativeActorRef failed_actor,
                     std::uint64_t failed_incarnation,
                     OresNativeActorRef replacement) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, failed_incarnation, Phase::finalized);
        validate_actor(replacement);
        if (same_actor(replacement, failed_actor)
            || active_actors_.count(actor_key(replacement)))
            throw std::logic_error("native replacement actor is not fresh");
        if (failed_incarnation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("logical child incarnation exhausted");
        auto& stored = children_.at(child.logical_child_id);
        stored.actor = replacement;
        stored.incarnation = failed_incarnation + 1;
        stored.phase = Phase::active;
        active_actors_.emplace(actor_key(replacement), child.logical_child_id);
        return stored;
    }

    Snapshot stop(ChildRef child, OresNativeActorRef failed_actor,
                  std::uint64_t incarnation) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_expected(child, failed_actor, incarnation, Phase::finalized);
        auto& stored = children_.at(child.logical_child_id);
        stored.phase = Phase::stopped;
        return stored;
    }

    void retire_stopped(ChildRef child) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto current = require_current(child);
        if (current.phase != Phase::stopped)
            throw std::logic_error("cannot retire active/restartable child");
        children_.erase(child.logical_child_id);
    }

    std::size_t tracked_children() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return children_.size();
    }

private:
    static bool same_actor(OresNativeActorRef a, OresNativeActorRef b) noexcept {
        return a.scheduler_domain == b.scheduler_domain
            && a.actor_slot == b.actor_slot && a.generation == b.generation;
    }
    static std::pair<std::uint64_t, std::uint64_t>
    actor_key(OresNativeActorRef actor) noexcept {
        return {actor.actor_slot, actor.generation};
    }
    void validate_actor(OresNativeActorRef actor) const {
        if (actor.scheduler_domain != runtime_id_ || !actor.generation)
            throw std::invalid_argument("foreign or invalid actor incarnation");
    }
    Snapshot require_current(ChildRef child) const {
        if (child.supervisor_id != supervisor_id_
            || child.runtime_id != runtime_id_ || !child.logical_child_id)
            throw std::invalid_argument("foreign supervisor child identity");
        auto found = children_.find(child.logical_child_id);
        if (found == children_.end() || !(found->second.child == child))
            throw std::logic_error("unknown or retired logical child");
        return found->second;
    }
    Snapshot require_expected(ChildRef child, OresNativeActorRef actor,
                              std::uint64_t incarnation, Phase phase) const {
        auto state = require_current(child);
        if (!same_actor(state.actor, actor) || state.incarnation != incarnation
            || state.phase != phase)
            throw std::logic_error("stale child incarnation or lifecycle phase");
        return state;
    }

    const std::uint64_t supervisor_id_;
    const std::uint64_t runtime_id_;
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 0;
    std::unordered_map<std::uint64_t, Snapshot> children_;
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             std::uint64_t> active_actors_;
};

} // namespace oreslang::runtime
