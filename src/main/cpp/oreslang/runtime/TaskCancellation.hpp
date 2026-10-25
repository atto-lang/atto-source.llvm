#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

class TaskCancellation final : public std::enable_shared_from_this<TaskCancellation> {
private:
    struct Registration;

public:
    using Ptr = std::shared_ptr<TaskCancellation>;

    class Subscription final {
    public:
        Subscription() = default;

        void close() const noexcept {
            if (registration_) registration_->close();
        }

        explicit operator bool() const noexcept { return bool(registration_); }

    private:
        friend class TaskCancellation;
        explicit Subscription(std::shared_ptr<Registration> registration)
            : registration_(std::move(registration)) {}

        std::shared_ptr<Registration> registration_;
    };

    static Ptr root() { return Ptr(new TaskCancellation()); }

    Ptr child() {
        std::lock_guard<std::mutex> guard(graph_mutex());
        if (retired_ || child_admission_sealed_)
            throw std::logic_error("cannot spawn from sealed task authority");

        Ptr result(new TaskCancellation());
        result->lifetime_parent_ = shared_from_this();
        lifetime_children_.emplace(result.get(), result);
        if (cancelled_) {
            result->cancellation_reason_ = cancellation_reason_;
            result->cancelled_ = true;
            result->revocation_complete_ = true;
        } else {
            result->owner_ = shared_from_this();
            children_.emplace(result.get(), result);
        }
        return result;
    }

    bool is_cancelled() const {
        std::lock_guard<std::mutex> guard(graph_mutex());
        return cancelled_;
    }

    std::string cancellation_reason() const {
        std::lock_guard<std::mutex> guard(graph_mutex());
        return cancellation_reason_;
    }
    /*
     * Native scheduler admission shares the same linearization point with
     * cancellation AND retirement. Only bounded, non-reentrant C store
     * operations may be supplied: never run guest code, call a Task method,
     * suspend, or retain this lock across a scheduler callback.
     */
    template <typename Operation>
    bool with_active_authority(Operation&& operation) const {
        std::lock_guard<std::mutex> guard(graph_mutex());
        if (retired_ || cancelled_) return false;
        return static_cast<bool>(std::forward<Operation>(operation)());
    }


    std::size_t owned_child_count() const {
        std::lock_guard<std::mutex> guard(graph_mutex());
        return children_.size();
    }

    bool is_owned_by(const Ptr& expected_owner) const {
        std::lock_guard<std::mutex> guard(graph_mutex());
        return owner_.lock() == expected_owner;
    }

    void adopt_into(const Ptr& new_owner) {
        if (!new_owner) throw std::invalid_argument("new owner is null");
        std::function<void()> notify_previous_owner;
        {
            std::lock_guard<std::mutex> guard(graph_mutex());
            if (retired_) throw std::logic_error("cannot transfer retired task");
            if (new_owner->retired_ || new_owner->child_admission_sealed_)
                throw std::logic_error("cannot adopt into sealed task owner");
            if (cancelled_) throw std::logic_error("cannot transfer cancelled task");
            if (new_owner->cancelled_)
                throw std::logic_error("cannot adopt into cancelled owner");

            Ptr current_owner = owner_.lock();
            if (!current_owner)
                throw std::logic_error("cannot transfer a root authority");

            for (Ptr cursor = new_owner; cursor; cursor = cursor->owner_.lock()) {
                if (cursor.get() == this)
                    throw std::invalid_argument("ownership cycle");
            }
            if (current_owner == new_owner) return;

            current_owner->children_.erase(this);
            new_owner->children_[this] = shared_from_this();
            owner_ = new_owner;

            notify_previous_owner = detach_lifetime_parent_locked();
            lifetime_parent_ = new_owner;
            new_owner->lifetime_children_[this] = shared_from_this();
        }
        if (notify_previous_owner) notify_previous_owner();
    }

    bool cancel(std::string reason = "cancelled") {
        validate_reason(reason);
        std::vector<Ptr> marked;
        std::vector<std::shared_ptr<Registration>> revokers;
        {
            std::lock_guard<std::mutex> guard(graph_mutex());
            if (retired_ || cancelled_) return false;

            std::deque<Ptr> queue;
            queue.push_back(shared_from_this());
            while (!queue.empty()) {
                Ptr node = std::move(queue.front());
                queue.pop_front();
                if (node->cancelled_) continue;

                node->cancellation_reason_ = reason;
                node->cancelled_ = true;
                for (const auto& entry : node->children_)
                    queue.push_back(entry.second);
                node->children_.clear();
                if (Ptr owner = node->owner_.lock()) owner->children_.erase(node.get());
                node->owner_.reset();
                append_registrations(node->revokers_, revokers);
                node->revokers_.clear();
                marked.push_back(std::move(node));
            }
        }

        dispatch([marked = std::move(marked), revokers = std::move(revokers)]() mutable {
            std::exception_ptr first_failure;
            deliver_all(revokers, first_failure);

            std::vector<std::shared_ptr<Registration>> observers;
            {
                std::lock_guard<std::mutex> guard(graph_mutex());
                for (const Ptr& node : marked) {
                    node->revocation_complete_ = true;
                    append_registrations(node->observers_, observers);
                    node->observers_.clear();
                }
            }
            deliver_all(observers, first_failure);
            if (first_failure) std::rethrow_exception(first_failure);
        });
        return true;
    }

    void retire() {
        std::vector<std::shared_ptr<Registration>> retire_revokers;
        std::vector<std::shared_ptr<Registration>> detached_observers;
        std::vector<std::shared_ptr<Registration>> detached_links;
        std::function<void()> notify_lifetime_parent;
        {
            std::lock_guard<std::mutex> guard(graph_mutex());
            if (retired_) return;
            if (!lifetime_children_.empty())
                throw std::logic_error("cannot retire authority with live children");

            retired_ = true;
            detached_links.swap(upstream_links_);
            if (Ptr owner = owner_.lock()) owner->children_.erase(this);
            owner_.reset();
            notify_lifetime_parent = detach_lifetime_parent_locked();

            /*
             * Retiring a Task authority must revoke its outstanding native
             * wait registrations, not silently orphan them. Cancellation
             * and retirement share one revocation linearization boundary.
             */
            append_registrations(revokers_, retire_revokers);
            revokers_.clear();
            if (!cancelled_ || revocation_complete_) {
                append_registrations(observers_, detached_observers);
                observers_.clear();
            }
        }

        std::exception_ptr first_failure;
        deliver_all(retire_revokers, first_failure);
        for (const auto& registration : detached_observers) registration->close();
        for (const auto& registration : detached_links) registration->close();
        if (notify_lifetime_parent) notify_lifetime_parent();
        if (first_failure) std::rethrow_exception(first_failure);
    }

    bool seal_and_observe_child_quiescence(std::function<void()> on_drained) {
        if (!on_drained) throw std::invalid_argument("child completion observer is empty");
        std::lock_guard<std::mutex> guard(graph_mutex());
        if (retired_) return true;
        if (child_admission_sealed_)
            throw std::logic_error("child completion observer already registered");
        child_admission_sealed_ = true;
        if (lifetime_children_.empty()) return true;
        children_drained_ = std::move(on_drained);
        return false;
    }

    Subscription on_revoke(std::function<void()> hook) {
        return register_hook(std::move(hook), true);
    }

    Subscription on_cancel(std::function<void()> hook) {
        return register_hook(std::move(hook), false);
    }

    Subscription link_cancellation_from(const Ptr& source) {
        if (!source) throw std::invalid_argument("cancellation source is null");
        if (source.get() == this) throw std::invalid_argument("self cancellation link");

        std::weak_ptr<TaskCancellation> weak_target = shared_from_this();
        std::weak_ptr<TaskCancellation> weak_source = source;
        Subscription link = source->on_cancel([weak_target, weak_source]() {
            Ptr target = weak_target.lock();
            Ptr source_node = weak_source.lock();
            if (!target || !source_node) return;
            std::string reason = source_node->cancellation_reason();
            if (reason.empty()) reason = "cancelled";
            if (reason.rfind("linked:", 0) != 0) reason = "linked:" + reason;
            if (reason.size() > max_reason_length) reason.resize(max_reason_length);
            target->cancel(std::move(reason));
        });

        bool discard = false;
        {
            std::lock_guard<std::mutex> guard(graph_mutex());
            discard = retired_;
            if (!discard && link.registration_) upstream_links_.push_back(link.registration_);
        }
        if (discard) link.close();
        return link;
    }

private:
    static constexpr std::size_t max_reason_length = 256;

    struct Registration final {
        std::weak_ptr<TaskCancellation> source;
        std::uint64_t id = 0;
        bool revoke = false;
        std::function<void()> hook;
        std::atomic<bool> active{true};

        void deliver() {
            if (!active.exchange(false, std::memory_order_acq_rel)) return;
            auto call = std::move(hook);
            source.reset();
            if (call) call();
        }

        void close() noexcept {
            if (!active.exchange(false, std::memory_order_acq_rel)) return;
            Ptr source_node = source.lock();
            if (source_node) {
                std::lock_guard<std::mutex> guard(graph_mutex());
                auto& registrations = revoke ? source_node->revokers_ : source_node->observers_;
                registrations.erase(id);
            }
            source.reset();
            hook = {};
        }
    };

    TaskCancellation() = default;

    Subscription register_hook(std::function<void()> hook, bool revoke) {
        if (!hook) throw std::invalid_argument("cancellation hook is empty");
        auto registration = std::make_shared<Registration>();
        registration->source = shared_from_this();
        registration->id = next_registration_id().fetch_add(1, std::memory_order_relaxed);
        registration->revoke = revoke;
        registration->hook = std::move(hook);

        bool deliver = false;
        bool discard = false;
        {
            std::lock_guard<std::mutex> guard(graph_mutex());
            /*
             * A registration racing retirement must also be revoked:
             * otherwise a stale retired authority could admit a new await.
             * Cancellation observers remain suppressed after retirement.
             */
            discard = retired_ && !revoke;
            deliver = (retired_ && revoke) ||
                      (!retired_ && cancelled_ && (revoke || revocation_complete_));
            if (!discard && !deliver) {
                (revoke ? revokers_ : observers_).emplace(registration->id, registration);
            }
        }
        if (discard) registration->close();
        else if (deliver) dispatch([registration] { registration->deliver(); });
        return Subscription(std::move(registration));
    }

    std::function<void()> detach_lifetime_parent_locked() {
        Ptr previous = lifetime_parent_.lock();
        lifetime_parent_.reset();
        if (!previous) return {};
        previous->lifetime_children_.erase(this);
        if (!previous->lifetime_children_.empty()) return {};
        auto ready = std::move(previous->children_drained_);
        previous->children_drained_ = {};
        return ready;
    }

    static void validate_reason(const std::string& reason) {
        const auto nonblank = reason.find_first_not_of(" \t\r\n\f\v");
        if (reason.empty() || nonblank == std::string::npos || reason.size() > max_reason_length)
            throw std::invalid_argument("reason must contain 1..256 nonblank characters");
    }

    static void append_registrations(
            const std::map<std::uint64_t, std::shared_ptr<Registration>>& source,
            std::vector<std::shared_ptr<Registration>>& destination) {
        destination.reserve(destination.size() + source.size());
        for (const auto& entry : source) destination.push_back(entry.second);
    }

    static void deliver_all(const std::vector<std::shared_ptr<Registration>>& hooks,
                            std::exception_ptr& first_failure) {
        for (const auto& hook : hooks) {
            try {
                hook->deliver();
            } catch (...) {
                if (!first_failure) first_failure = std::current_exception();
            }
        }
    }

    static void dispatch(std::function<void()> action) {
        static thread_local bool draining = false;
        static thread_local std::deque<std::function<void()>> queue;
        if (draining) {
            queue.push_back(std::move(action));
            return;
        }

        draining = true;
        queue.push_back(std::move(action));
        std::exception_ptr first_failure;
        while (!queue.empty()) {
            auto current = std::move(queue.front());
            queue.pop_front();
            try {
                current();
            } catch (...) {
                if (!first_failure) first_failure = std::current_exception();
            }
        }
        draining = false;
        if (first_failure) std::rethrow_exception(first_failure);
    }

    static std::mutex& graph_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::atomic<std::uint64_t>& next_registration_id() {
        static std::atomic<std::uint64_t> next{1};
        return next;
    }

    std::unordered_map<TaskCancellation*, Ptr> children_;
    std::unordered_map<TaskCancellation*, Ptr> lifetime_children_;
    // Registration ids are monotonic; ordered maps preserve deterministic
    // same-authority hook delivery and match Java's LinkedHashSet contract.
    std::map<std::uint64_t, std::shared_ptr<Registration>> revokers_;
    std::map<std::uint64_t, std::shared_ptr<Registration>> observers_;
    std::vector<std::shared_ptr<Registration>> upstream_links_;
    std::weak_ptr<TaskCancellation> owner_;
    std::weak_ptr<TaskCancellation> lifetime_parent_;
    std::function<void()> children_drained_;
    bool child_admission_sealed_ = false;
    bool retired_ = false;
    bool cancelled_ = false;
    bool revocation_complete_ = false;
    std::string cancellation_reason_;
};

}  // namespace oreslang::runtime
