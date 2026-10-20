#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

// Internal source for globally unique registry incarnation identifiers.
// Never wrap: silently reissuing an old identifier would let a stale join
// capability regain authority in a newly constructed registry.
namespace detail {
inline std::uint64_t take_nonwrapping_registry_id(
        std::atomic<std::uint64_t>& sequence) {
    std::uint64_t current = sequence.load(std::memory_order_relaxed);
    for (;;) {
        if (current == 0 || current == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor membership registry IDs exhausted");
        if (sequence.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return current;
    }
}
} // namespace detail

/**
 * Trusted native runtime cooperation-membership registry.
 *
 * Membership is NOT NativeActorGroup::adopt(): one actor incarnation may join
 * many cooperation groups, but the independent NativeActorGroup lifecycle
 * registry still grants exclusive stop/cancel/finalization authority.
 *
 * The trusted actor runtime must register actors after creation and retire
 * them only after termination. This is a CONTROL-plane primitive, not a guest
 * capability ABI or source-level ActorGroup lowering.
 */
class NativeActorMembershipRegistry final {
public:
    class JoinCapability final {
    public:
        std::uint64_t group_id() const noexcept { return group_id_; }
    private:
        friend class NativeActorMembershipRegistry;
        JoinCapability(std::uint64_t registry_id, std::uint64_t group_id,
                       std::uint64_t epoch)
            : registry_id_(registry_id), group_id_(group_id), epoch_(epoch) {}
        const std::uint64_t registry_id_;
        const std::uint64_t group_id_;
        const std::uint64_t epoch_;
    };

    explicit NativeActorMembershipRegistry(std::uint64_t runtime_id,
                                           std::size_t max_groups,
                                           std::size_t max_groups_per_actor)
        : runtime_id_(runtime_id),
          max_groups_(max_groups),
          max_groups_per_actor_(max_groups_per_actor),
          instance_id_(detail::take_nonwrapping_registry_id(next_instance_id())) {
        if (runtime_id_ == 0 || max_groups_ == 0
            || max_groups_per_actor_ == 0 || instance_id_ == 0)
            throw std::invalid_argument("invalid actor membership registry limits");
    }

    NativeActorMembershipRegistry(const NativeActorMembershipRegistry&) = delete;
    NativeActorMembershipRegistry& operator=(const NativeActorMembershipRegistry&) = delete;

    std::uint64_t runtime_id() const noexcept { return runtime_id_; }

    // Trusted runtime registration. An actor ID may not be reused while the
    // prior incarnation is live, even if all its group memberships are gone.
    bool register_actor(std::uint64_t actor_id, std::uint64_t generation) {
        if (!actor_id || !generation)
            throw std::invalid_argument("invalid actor identity");
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = actors_.find(actor_id);
        if (it != actors_.end()) {
            if (it->second.generation != generation)
                throw std::logic_error("actor ID still belongs to a live incarnation");
            return false;
        }
        actors_.emplace(actor_id, ActorRecord{generation, {}});
        return true;
    }

    JoinCapability create_group(std::uint64_t group_id, std::size_t max_members) {
        if (!group_id || !max_members)
            throw std::invalid_argument("invalid cooperation group limits");
        std::lock_guard<std::mutex> guard(mutex_);
        if (groups_.find(group_id) != groups_.end())
            throw std::logic_error("duplicate cooperation group ID");
        if (groups_.size() >= max_groups_)
            throw std::logic_error("cooperation group registry capacity exceeded");
        const std::uint64_t epoch = next_epoch_locked();
        groups_.emplace(group_id, GroupRecord{epoch, max_members, {}});
        return JoinCapability(instance_id_, group_id, epoch);
    }

    // Rotate only future admission. Existing members and their subscriptions
    // remain members until leave, group close, or actor retirement.
    JoinCapability rotate_join(const JoinCapability& capability) {
        std::lock_guard<std::mutex> guard(mutex_);
        GroupRecord& group = require_group_locked(capability);
        group.epoch = next_epoch_locked();
        return JoinCapability(instance_id_, capability.group_id_, group.epoch);
    }

    // True if newly joined; false for an idempotent duplicate join.
    // Lock serializes per-actor quota across every group and joins versus close.
    bool join(const JoinCapability& capability, std::uint64_t actor_id,
              std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        GroupRecord& group = require_group_locked(capability);
        auto actor = actors_.find(actor_id);
        if (actor == actors_.end() || actor->second.generation != generation)
            throw std::logic_error("unknown or stale actor incarnation");
        if (actor->second.groups.count(capability.group_id_)) return false;
        if (actor->second.groups.size() >= max_groups_per_actor_)
            throw std::logic_error("actor cooperation membership quota exceeded");
        if (group.members.size() >= group.max_members)
            throw std::logic_error("cooperation group capacity exceeded");
        const ActorKey key{actor_id, generation};
        group.members.insert(key);
        try {
            actor->second.groups.insert(capability.group_id_);
        } catch (...) {
            group.members.erase(key); // strong exception safety across indexes
            throw;
        }
        return true;
    }

    // Trusted runtime leave: clears only this group's membership. Never
    // stops the actor or affects any of its other cooperation groups.
    bool leave(std::uint64_t group_id, std::uint64_t actor_id,
               std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto group = groups_.find(group_id);
        auto actor = actors_.find(actor_id);
        if (group == groups_.end() || actor == actors_.end()
            || actor->second.generation != generation) return false;
        if (!actor->second.groups.erase(group_id)) return false;
        group->second.members.erase(ActorKey{actor_id, generation});
        return true;
    }

    // Close/revoke a cooperation domain without any lifecycle stop requests.
    // A recreated group ID always receives a fresh epoch; old permits fail.
    bool close_group(std::uint64_t group_id) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) return false;
        for (const auto& member : it->second.members) {
            auto actor = actors_.find(member.first);
            if (actor != actors_.end() && actor->second.generation == member.second)
                actor->second.groups.erase(group_id);
        }
        groups_.erase(it);
        return true;
    }

    // Physical actor retirement is generation-fenced: late acknowledgments
    // cannot remove the next incarnation's memberships.
    bool retire_actor(std::uint64_t actor_id, std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = actors_.find(actor_id);
        if (it == actors_.end() || it->second.generation != generation)
            return false;
        for (auto group_id : it->second.groups) {
            auto group = groups_.find(group_id);
            if (group != groups_.end())
                group->second.members.erase(ActorKey{actor_id, generation});
        }
        actors_.erase(it);
        return true;
    }

    bool contains(std::uint64_t group_id, std::uint64_t actor_id,
                  std::uint64_t generation) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = groups_.find(group_id);
        return it != groups_.end()
            && it->second.members.count(ActorKey{actor_id, generation}) != 0;
    }

    std::size_t member_count(std::uint64_t group_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = groups_.find(group_id);
        return it == groups_.end() ? 0 : it->second.members.size();
    }

    std::size_t membership_count(std::uint64_t actor_id,
                                 std::uint64_t generation) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = actors_.find(actor_id);
        return (it == actors_.end() || it->second.generation != generation)
            ? 0 : it->second.groups.size();
    }

private:
    using ActorKey = std::pair<std::uint64_t, std::uint64_t>;
    struct ActorRecord {
        std::uint64_t generation;
        std::set<std::uint64_t> groups;
    };
    struct GroupRecord {
        std::uint64_t epoch;
        std::size_t max_members;
        std::set<ActorKey> members;
    };

    static std::atomic<std::uint64_t>& next_instance_id() {
        static std::atomic<std::uint64_t> sequence{1};
        return sequence;
    }

    std::uint64_t next_epoch_locked() {
        if (next_epoch_ == 0 || next_epoch_ == UINT64_MAX)
            throw std::overflow_error("cooperation group epoch exhausted");
        return next_epoch_++;
    }

    GroupRecord& require_group_locked(const JoinCapability& capability) {
        if (capability.registry_id_ != instance_id_)
            throw std::invalid_argument("foreign actor membership capability");
        auto it = groups_.find(capability.group_id_);
        if (it == groups_.end() || it->second.epoch != capability.epoch_)
            throw std::logic_error("unknown or revoked group join capability");
        return it->second;
    }

    const std::uint64_t runtime_id_;
    const std::size_t max_groups_;
    const std::size_t max_groups_per_actor_;
    const std::uint64_t instance_id_;
    mutable std::mutex mutex_;
    std::uint64_t next_epoch_ = 1;
    std::map<std::uint64_t, ActorRecord> actors_;
    std::map<std::uint64_t, GroupRecord> groups_;
};

} // namespace oreslang::runtime
