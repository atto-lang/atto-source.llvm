#include "oreslang/runtime/TaskCancellation.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using oreslang::runtime::TaskCancellation;

static void propagation_and_first_reason() {
    auto root = TaskCancellation::root();
    auto child = root->child();
    auto grandchild = child->child();
    assert(root->cancel("shutdown"));
    assert(root->is_cancelled());
    assert(child->is_cancelled());
    assert(grandchild->is_cancelled());
    assert(child->cancellation_reason() == "shutdown");
    assert(!root->cancel("second"));
    assert(root->cancellation_reason() == "shutdown");
}

static void child_cancel_is_one_way() {
    auto root = TaskCancellation::root();
    auto left = root->child();
    auto right = root->child();
    assert(left->cancel("left-only"));
    assert(!root->is_cancelled());
    assert(!right->is_cancelled());
}

static void cancelled_parent_child_inherits() {
    auto root = TaskCancellation::root();
    assert(root->cancel("already-stopped"));
    auto child = root->child();
    assert(child->is_cancelled());
    assert(child->cancellation_reason() == "already-stopped");
    int notifications = 0;
    child->on_cancel([&] { ++notifications; });
    assert(notifications == 1);
}

static void adoption_and_cycle_rejection() {
    auto root = TaskCancellation::root();
    auto other = TaskCancellation::root();
    auto child = root->child();
    auto descendant = child->child();
    child->adopt_into(other);
    assert(child->is_owned_by(other));
    assert(root->owned_child_count() == 0);
    assert(other->owned_child_count() == 1);
    bool cycle_rejected = false;
    try {
        child->adopt_into(descendant);
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    assert(cycle_rejected);
}

static void child_lifetime_join_barrier() {
    auto root = TaskCancellation::root();
    auto child = root->child();
    int drained = 0;
    assert(!root->seal_and_observe_child_quiescence([&] { ++drained; }));
    bool spawn_rejected = false;
    try {
        (void)root->child();
    } catch (const std::logic_error&) {
        spawn_rejected = true;
    }
    assert(spawn_rejected);
    child->retire();
    assert(drained == 1);
    root->retire();
}

static void cannot_retire_with_live_child() {
    auto root = TaskCancellation::root();
    auto child = root->child();
    bool rejected = false;
    try {
        root->retire();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
    child->retire();
    root->retire();
}

static void revokers_precede_observers_and_run_unlocked() {
    auto root = TaskCancellation::root();
    std::vector<int> order;
    root->on_revoke([&] {
        order.push_back(1);
        auto inherited = root->child();
        assert(inherited->is_cancelled());
    });
    root->on_cancel([&] { order.push_back(2); });
    assert(root->cancel("ordered"));
    assert((order == std::vector<int>{1, 2}));
}

static void registration_order_is_deterministic() {
    auto root = TaskCancellation::root();
    std::vector<int> order;
    root->on_cancel([&] { order.push_back(4); });
    root->on_revoke([&] { order.push_back(1); });
    root->on_revoke([&] { order.push_back(2); });
    root->on_revoke([&] { order.push_back(3); });
    assert(root->cancel("ordered"));
    assert((order == std::vector<int>{1, 2, 3, 4}));
}

static void descendants_are_marked_before_callbacks() {
    auto root = TaskCancellation::root();
    auto left = root->child();
    auto right = root->child();
    bool sibling_already_cancelled = false;
    left->on_revoke([&] { sibling_already_cancelled = right->is_cancelled(); });
    assert(root->cancel("subtree"));
    assert(sibling_already_cancelled);
}

static void late_observer_waits_for_revocation_barrier() {
    auto root = TaskCancellation::root();
    std::atomic<bool> revoker_entered{false};
    std::atomic<bool> release_revoker{false};
    std::atomic<int> observed{0};
    root->on_revoke([&] {
        revoker_entered.store(true, std::memory_order_release);
        while (!release_revoker.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    std::thread canceller([&] { assert(root->cancel("barrier")); });
    while (!revoker_entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    root->on_cancel([&] { observed.fetch_add(1, std::memory_order_relaxed); });
    assert(observed.load(std::memory_order_relaxed) == 0);
    release_revoker.store(true, std::memory_order_release);
    canceller.join();
    assert(observed.load(std::memory_order_relaxed) == 1);
}

static void cancellation_and_adoption_race_is_linearized() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto old_owner = TaskCancellation::root();
        auto new_owner = TaskCancellation::root();
        auto child = old_owner->child();
        std::atomic<bool> go{false};
        std::atomic<bool> adopted{false};
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            old_owner->cancel("old");
        });
        std::thread adopter([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                child->adopt_into(new_owner);
                adopted.store(true, std::memory_order_release);
            } catch (const std::logic_error&) {
            }
        });
        go.store(true, std::memory_order_release);
        canceller.join();
        adopter.join();

        if (adopted.load(std::memory_order_acquire)) {
            assert(child->is_owned_by(new_owner));
            assert(!child->is_cancelled());
        } else {
            assert(child->is_cancelled());
            assert(child->cancellation_reason() == "old");
        }
        new_owner->cancel("new");
        assert(child->is_cancelled());
    }
}

static void retirement_during_revocation_preserves_observer() {
    auto root = TaskCancellation::root();
    std::atomic<int> observed{0};
    root->on_cancel([&] { observed.fetch_add(1, std::memory_order_relaxed); });
    root->on_revoke([&] { root->retire(); });
    assert(root->cancel("retire-during-revoke"));
    assert(observed.load(std::memory_order_relaxed) == 1);
    root->on_cancel([&] { observed.fetch_add(100, std::memory_order_relaxed); });
    assert(observed.load(std::memory_order_relaxed) == 1);
}

static void detached_subscription_does_not_fire() {
    auto root = TaskCancellation::root();
    int calls = 0;
    auto subscription = root->on_cancel([&] { ++calls; });
    subscription.close();
    assert(root->cancel("detached"));
    assert(calls == 0);
}

static void directional_link_and_unlink() {
    auto source = TaskCancellation::root();
    auto target = TaskCancellation::root();
    auto link = target->link_cancellation_from(source);
    assert(source->cancel("upstream"));
    assert(target->is_cancelled());
    assert(target->cancellation_reason() == "linked:upstream");

    auto source2 = TaskCancellation::root();
    auto target2 = TaskCancellation::root();
    auto detached = target2->link_cancellation_from(source2);
    detached.close();
    assert(source2->cancel("ignored"));
    assert(!target2->is_cancelled());
}

static void retirement_closes_upstream_link() {
    auto source = TaskCancellation::root();
    auto target = TaskCancellation::root();
    (void)target->link_cancellation_from(source);
    target->retire();
    assert(source->cancel("after-retire"));
    assert(!target->is_cancelled());
}

static void adoption_moves_lifetime_join_barrier() {
    auto old_owner = TaskCancellation::root();
    auto new_owner = TaskCancellation::root();
    auto child = old_owner->child();
    int old_drained = 0;
    assert(!old_owner->seal_and_observe_child_quiescence([&] { ++old_drained; }));
    child->adopt_into(new_owner);
    assert(old_drained == 1);
    assert(child->is_owned_by(new_owner));
    child->retire();
    old_owner->retire();
    new_owner->retire();
}

static void throwing_revoker_does_not_strand_siblings_or_observers() {
    auto root = TaskCancellation::root();
    int revokers = 0;
    int observers = 0;
    root->on_revoke([&] {
        ++revokers;
        throw std::runtime_error("trusted revoker failed");
    });
    root->on_revoke([&] { ++revokers; });
    root->on_cancel([&] { ++observers; });
    bool rethrown = false;
    try {
        root->cancel("drain-all");
    } catch (const std::runtime_error&) {
        rethrown = true;
    }
    assert(rethrown);
    assert(revokers == 2);
    assert(observers == 1);
    assert(root->is_cancelled());
}

static void concurrent_first_reason_wins() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto root = TaskCancellation::root();
        std::atomic<int> winners{0};
        std::thread a([&] { if (root->cancel("alpha")) ++winners; });
        std::thread b([&] { if (root->cancel("beta")) ++winners; });
        a.join();
        b.join();
        assert(winners.load() == 1);
        const auto reason = root->cancellation_reason();
        assert(reason == "alpha" || reason == "beta");
    }
}

static void deep_tree_cancels_iteratively() {
    auto root = TaskCancellation::root();
    auto cursor = root;
    std::vector<TaskCancellation::Ptr> keep_alive;
    keep_alive.push_back(root);
    for (int i = 0; i < 4096; ++i) {
        cursor = cursor->child();
        keep_alive.push_back(cursor);
    }
    assert(root->cancel("deep"));
    assert(keep_alive.back()->is_cancelled());
}

static void retirement_revokes_waiters_without_firing_cancel_observers() {
    auto task = TaskCancellation::root();
    int revocations = 0;
    int cancellations = 0;
    task->on_revoke([&] { ++revocations; });
    task->on_cancel([&] { ++cancellations; });
    task->retire();
    assert(revocations == 1);
    assert(cancellations == 0);
    task->on_revoke([&] { ++revocations; });
    task->on_cancel([&] { ++cancellations; });
    assert(revocations == 2);
    assert(cancellations == 0);
}

static void retirement_drains_throwing_revoker_before_observer_cleanup() {
    auto task = TaskCancellation::root();
    int revocations = 0;
    task->on_revoke([&] {
        ++revocations;
        throw std::runtime_error("host tear-down failure");
    });
    task->on_revoke([&] { ++revocations; });
    bool threw = false;
    try {
        task->retire();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    assert(revocations == 2);
    // Idempotent, despite the recorded hook failure.
    task->retire();
}

static void admission_and_retirement_have_one_linearization_point() {
    auto task = TaskCancellation::root();
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> retired{false};
    std::thread admitted([&] {
        assert(task->with_active_authority([&] {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
            return true;
        }));
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::thread retire([&] {
        task->retire();
        retired.store(true, std::memory_order_release);
    });
    assert(!retired.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    admitted.join();
    retire.join();
    assert(retired.load(std::memory_order_acquire));
    bool invoked = false;
    assert(!task->with_active_authority([&] {
        invoked = true;
        return true;
    }));
    assert(!invoked);
}

static void invalid_reasons_fail_without_cancelling() {
    auto root = TaskCancellation::root();
    for (const std::string& invalid :
            {std::string{}, std::string("  \t\n"), std::string(257, 'x')}) {
        bool rejected = false;
        try {
            root->cancel(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(!root->is_cancelled());
    }
}

int main() {
    propagation_and_first_reason();
    child_cancel_is_one_way();
    cancelled_parent_child_inherits();
    adoption_and_cycle_rejection();
    child_lifetime_join_barrier();
    cannot_retire_with_live_child();
    revokers_precede_observers_and_run_unlocked();
    registration_order_is_deterministic();
    descendants_are_marked_before_callbacks();
    late_observer_waits_for_revocation_barrier();
    cancellation_and_adoption_race_is_linearized();
    retirement_during_revocation_preserves_observer();
    detached_subscription_does_not_fire();
    directional_link_and_unlink();
    retirement_closes_upstream_link();
    adoption_moves_lifetime_join_barrier();
    throwing_revoker_does_not_strand_siblings_or_observers();
    concurrent_first_reason_wins();
    deep_tree_cancels_iteratively();
    retirement_revokes_waiters_without_firing_cancel_observers();
    retirement_drains_throwing_revoker_before_observer_cleanup();
    admission_and_retirement_have_one_linearization_point();
    invalid_reasons_fail_without_cancelling();
}
