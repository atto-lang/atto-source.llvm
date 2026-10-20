#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Host CONTROL-plane supervision-tree registry.
 *
 * There is no OS process or dedicated carrier per supervisor. Stable
 * registered identities form an immutable tree. Each group is constructed
 * under one supervisor before it can be returned to any caller.
 *
 * This host registry is NOT compiler-lowered `define supervisor actor`,
 * and its handles do not confer source actor lifecycle authority. A host
 * CONTROL admission check is required at the embedding boundary.
 */
class NativeSupervisorRegistry final {
public:
    class SupervisorRef final {
    public:
        std::uint64_t id() const noexcept { return id_; }
    private:
        friend class NativeSupervisorRegistry;
        SupervisorRef(std::uint64_t id, std::shared_ptr<const unsigned char> domain)
            : id_(id), domain_(std::move(domain)) {}
        std::uint64_t id_;
        std::shared_ptr<const unsigned char> domain_;
    };

    explicit NativeSupervisorRegistry(std::uint64_t runtime_id,
            std::size_t maximum_supervisors = 65536,
            std::size_t maximum_groups = 65536)
        : runtime_id_(runtime_id), maximum_supervisors_(maximum_supervisors),
          maximum_groups_(maximum_groups),
          domain_(std::make_shared<const unsigned char>(0)) {
        if (runtime_id == 0 || maximum_supervisors == 0 || maximum_groups == 0)
            throw std::invalid_argument("invalid supervisor registry capacity/runtime");
        supervisors_.emplace(NativeActorGroup::root_supervisor_id, 0);
    }

    SupervisorRef root() const {
        return SupervisorRef(NativeActorGroup::root_supervisor_id, domain_);
    }

    SupervisorRef create_child(const SupervisorRef& parent) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(parent);
        if (supervisors_.size() >= maximum_supervisors_
            || next_supervisor_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("native supervisor capacity exhausted");
        const auto id = next_supervisor_id_++;
        supervisors_.emplace(id, parent.id());
        return SupervisorRef(id, domain_);
    }

    /**
     * Host CONTROL boundary. Binds the slot to the exact registry domain
     * and supervisor identity; a code revision changes policy, never group
     * parentage or group IDs. No guest may call this API directly.
     */
    void install_policy(const SupervisorRef& supervisor,
            std::shared_ptr<NativeSupervisorCodeSlot<std::string>> code) {
        if (!code) throw std::invalid_argument("null native supervisor policy code");
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        if (policies_.count(supervisor.id()))
            throw std::logic_error("native supervisor policy already installed");
        (void)code->current();
        code->bind_owner(domain_.get(), supervisor.id());
        policies_.emplace(supervisor.id(), std::move(code));
    }

    NativeSupervisorCodeSlot<std::string>::Descriptor policy_revision(
            const SupervisorRef& supervisor) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        auto it = policies_.find(supervisor.id());
        if (it == policies_.end())
            throw std::logic_error("supervisor has no installed policy code");
        return it->second->current();
    }

    std::uint64_t parent_id(const SupervisorRef& supervisor) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        return supervisors_.at(supervisor.id());
    }

    std::vector<std::uint64_t> child_ids(const SupervisorRef& supervisor) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        std::vector<std::uint64_t> result;
        for (const auto& entry : supervisors_)
            if (entry.second == supervisor.id()) result.push_back(entry.first);
        return result;
    }

    /**
     * Every public group return is already bound and published. The registry
     * owns only a weak reference: dropping the last external group handle
     * tears down the group and cannot retain actor heaps through the tree.
     */
    std::shared_ptr<NativeActorGroup> create_group(
            const SupervisorRef& supervisor, std::size_t maximum_workers) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        prune_closed_groups();
        if (groups_.size() >= maximum_groups_)
            throw std::length_error("native supervisor group capacity exhausted");
        auto group = std::make_shared<NativeActorGroup>(
                runtime_id_, maximum_workers, supervisor.id());
        groups_.emplace(group->group_id(),
                Association{supervisor.id(), group});
        return group;
    }

    std::vector<std::uint64_t> direct_group_ids(
            const SupervisorRef& supervisor) const {
        return group_ids(supervisor, false);
    }

    std::vector<std::uint64_t> subtree_group_ids(
            const SupervisorRef& supervisor) const {
        return group_ids(supervisor, true);
    }

private:
    struct Association {
        std::uint64_t supervisor_id;
        std::weak_ptr<NativeActorGroup> group;
    };

    void require_registered(const SupervisorRef& ref) const {
        if (ref.domain_.get() != domain_.get()
            || supervisors_.find(ref.id()) == supervisors_.end())
            throw std::invalid_argument(
                    "foreign or unregistered native supervisor identity");
    }

    bool is_descendant(std::uint64_t node, std::uint64_t ancestor) const {
        while (node != 0) {
            if (node == ancestor) return true;
            node = supervisors_.at(node);
        }
        return false;
    }

    std::vector<std::uint64_t> group_ids(const SupervisorRef& supervisor,
            bool include_descendants) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(supervisor);
        std::vector<std::uint64_t> result;
        for (const auto& entry : groups_) {
            const auto& association = entry.second;
            if (!(include_descendants
                    ? is_descendant(association.supervisor_id, supervisor.id())
                    : association.supervisor_id == supervisor.id()))
                continue;
            auto group = association.group.lock();
            if (group && !group->closed()) result.push_back(entry.first);
        }
        return result;
    }

    void prune_closed_groups() {
        for (auto it = groups_.begin(); it != groups_.end();) {
            auto group = it->second.group.lock();
            if (!group || group->closed()) it = groups_.erase(it);
            else ++it;
        }
    }

    const std::uint64_t runtime_id_;
    const std::size_t maximum_supervisors_;
    const std::size_t maximum_groups_;
    const std::shared_ptr<const unsigned char> domain_;
    mutable std::mutex mutex_;
    std::uint64_t next_supervisor_id_ = NativeActorGroup::root_supervisor_id + 1;
    std::unordered_map<std::uint64_t, std::uint64_t> supervisors_;
    std::unordered_map<std::uint64_t,
        std::shared_ptr<NativeSupervisorCodeSlot<std::string>>> policies_;
    std::unordered_map<std::uint64_t, Association> groups_;
};

} // namespace oreslang::runtime
