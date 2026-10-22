#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"
#include "oreslang/runtime/SupervisorLifecycleFsm.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <algorithm>
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
            std::size_t maximum_groups = 65536,
            std::size_t restart_budget = 0)
        : runtime_id_(runtime_id), maximum_supervisors_(maximum_supervisors),
          maximum_groups_(maximum_groups), restart_budget_(restart_budget),
          domain_(std::make_shared<const unsigned char>(0)) {
        if (runtime_id == 0 || maximum_supervisors == 0 || maximum_groups == 0)
            throw std::invalid_argument("invalid supervisor registry capacity/runtime");
        supervisors_.emplace(NativeActorGroup::root_supervisor_id, 0);
        lifecycles_.emplace(NativeActorGroup::root_supervisor_id, SupervisorLifecycleFsm::State{});
        controls_.emplace(NativeActorGroup::root_supervisor_id,
                std::make_shared<oreslang::SupervisorControlTurn>(maximum_supervisors, restart_budget_));
    }

    SupervisorRef root() const {
        return SupervisorRef(NativeActorGroup::root_supervisor_id, domain_);
    }

    SupervisorRef create_child(const SupervisorRef& parent) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(parent);
        require_admissions_open();
        std::size_t siblings = 0;
        for (const auto& node : supervisors_) if (node.second == parent.id()) ++siblings;
        if (siblings >= SupervisorLifecycleFsm::max_children)
            throw std::length_error("supervisor lifecycle child capacity exhausted");
        if (supervisors_.size() >= maximum_supervisors_
            || next_supervisor_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("native supervisor capacity exhausted");
        const auto id = next_supervisor_id_++;
        auto control = std::make_shared<oreslang::SupervisorControlTurn>(maximum_supervisors_, restart_budget_);
        lifecycles_.emplace(id, SupervisorLifecycleFsm::State{});
        try {
            controls_.emplace(id, std::move(control));
            supervisors_.emplace(id, parent.id());
        } catch (...) {
            controls_.erase(id);
            lifecycles_.erase(id);
            throw;
        }
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
        require_admissions_open();
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
        require_admissions_open();
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

    // Route host policy admissions through this registry-owned CONTROL lane.
    std::shared_ptr<oreslang::SupervisorControlTurn> control_lane(const SupervisorRef& ref) const {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(ref);
        return controls_.at(ref.id());
    }

    struct NodeStatus {
        std::uint64_t id, parent;
        SupervisorLifecycleFsm::State lifecycle;
    };
    struct TreeStatus {
        std::vector<NodeStatus> nodes;
        bool admissions_fenced, shutdown_committed;
        bool all_logically_stopped, all_physically_reclaimed;
        bool has_failures, has_deadlines;
    };
    TreeStatus status() const {
        std::lock_guard<std::mutex> guard(mutex_);
        TreeStatus result{{}, shutting_down_, shutdown_committed_, true, true, false, false};
        result.nodes.reserve(supervisors_.size());
        for (const auto& node : supervisors_) {
            const auto& state = lifecycles_.at(node.first);
            result.nodes.push_back({node.first, node.second, state});
            result.all_logically_stopped &= state.logically_stopped();
            result.all_physically_reclaimed &= state.phase == SupervisorLifecycleFsm::Phase::reclaimed;
            result.has_failures |= state.failed;
            result.has_deadlines |= state.timed_out;
        }
        std::sort(result.nodes.begin(), result.nodes.end(),
                [](const auto& a, const auto& b) { return a.id < b.id; });
        return result; // detached coherent value, no live mutable state references
    }

    // Seal membership/spawn before hooks. Busy CONTROL lanes return false;
    // retry on the existing coordinator lane. Previously fenced lanes stay
    // fenced on retry/failure. No guest callback executes under registry lock.
    bool begin_shutdown() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (shutdown_committed_) return true;
        shutting_down_ = true;
        for (auto& association : groups_)
            if (auto group = association.second.group.lock()) group->fence_worker_admissions();
        bool ready = true;
        for (auto& control : controls_)
            if (!control.second->try_fence_shutdown()) ready = false;
        if (!ready) return false;
        auto next = lifecycles_; // allocate before publishing any lifecycle state
        using F = SupervisorLifecycleFsm;
        // Build adjacency once: avoid a full registry scan for each node.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> children;
        children.reserve(supervisors_.size());
        for (const auto& node : supervisors_)
            if (node.second) children.emplace_back(node.second, node.first);
        std::sort(children.begin(), children.end());
        for (auto& node : next) {
            F::Event event{F::Kind::shutdown_requested, node.second.epoch};
            // Stable IDs increase in registration order. Canonical child order
            // makes reverse iteration available to a future hook coordinator.
            auto child = std::lower_bound(children.begin(), children.end(),
                    std::make_pair(node.first, std::uint64_t{0}));
            for (; child != children.end() && child->first == node.first; ++child)
                event.children[event.child_count++] = {child->second, lifecycles_.at(child->second).epoch};
            node.second = F::reduce(node.second, event);
        }
        lifecycles_.swap(next);
        shutdown_committed_ = true;
        return true;
    }

    // Trusted host hook acknowledgments carry the epoch captured at admission.
    // Child truth is checked against the registered subtree, not caller claims.
    // Physical issuers and runtime restart are deliberately not wired yet.
    void observe(const SupervisorRef& ref, const SupervisorLifecycleFsm::Event& event) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_registered(ref);
        if (!shutdown_committed_) throw std::logic_error("shutdown fence not committed");
        using F = SupervisorLifecycleFsm;
        auto& state = lifecycles_.at(ref.id());
        if (event.epoch < state.epoch) return;
        if (event.epoch != state.epoch) throw std::logic_error("future supervisor epoch");
        if (event.kind == F::Kind::physical_release || event.kind == F::Kind::child_reclaimed
                || event.kind == F::Kind::restart_requested || event.kind == F::Kind::restart_committed)
            throw std::logic_error("native physical/restart receipt issuer not integrated");
        if (event.kind == F::Kind::before_finished) {
            auto parent = supervisors_.at(ref.id());
            if (parent && !lifecycles_.at(parent).before_done)
                throw std::logic_error("child hook before parent pre hook");
        }
        auto checked = event;
        if (event.kind == F::Kind::child_finished) {
            auto it = supervisors_.find(event.child.id);
            if (it == supervisors_.end() || it->second != ref.id())
                throw std::logic_error("foreign child acknowledgment");
            const auto& child = lifecycles_.at(event.child.id);
            if (event.child.incarnation < child.epoch) return;
            if (event.child.incarnation != child.epoch || !child.logically_stopped())
                throw std::logic_error("child not logically stopped in captured incarnation");
            checked.failed |= child.failed;
        }
        if (event.kind == F::Kind::actors_finished) {
            for (const auto& group : groups_)
                if (group.second.supervisor_id == ref.id()
                        && NativeActorGroup::has_live_managed_workers(group.first))
                    throw std::logic_error("managed actor still physically live");
        }
        state = F::reduce(state, checked);
    }

private:
    void require_admissions_open() const {
        if (shutting_down_) throw std::logic_error("supervisor tree admission fenced");
    }
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
            if ((!group || group->closed())
                    && !NativeActorGroup::has_live_managed_workers(it->first)) it = groups_.erase(it);
            else ++it;
        }
    }

    const std::uint64_t runtime_id_;
    const std::size_t maximum_supervisors_;
    const std::size_t maximum_groups_;
    const std::size_t restart_budget_;
    const std::shared_ptr<const unsigned char> domain_;
    mutable std::mutex mutex_;
    std::uint64_t next_supervisor_id_ = NativeActorGroup::root_supervisor_id + 1;
    std::unordered_map<std::uint64_t, std::uint64_t> supervisors_;
    std::unordered_map<std::uint64_t, SupervisorLifecycleFsm::State> lifecycles_;
    std::unordered_map<std::uint64_t, std::shared_ptr<oreslang::SupervisorControlTurn>> controls_;
    bool shutting_down_ = false;
    bool shutdown_committed_ = false;
    std::unordered_map<std::uint64_t,
        std::shared_ptr<NativeSupervisorCodeSlot<std::string>>> policies_;
    std::unordered_map<std::uint64_t, Association> groups_;
};

} // namespace oreslang::runtime

