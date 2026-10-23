#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeActorRef;
using namespace std::chrono_literals;

static void check(bool yes, const char* error) {
    if (!yes) throw std::runtime_error(error);
}

int main() {
    {
        NativeActorGroup group(6101, 4);
        auto parent = group.adopt(6101, 1, 1, [] {});
        auto child = group.adopt_child(parent, 6101, 2, 1, [] {});
        auto grandchild = group.adopt_child(child, 6101, 3, 1, [] {});
        check(!group.spawned_by(parent).has_value(), "root acquired fabricated parent");
        check(group.spawned_by(child) ==
                  std::make_optional(std::pair<std::uint64_t, std::uint64_t>{1, 1}),
              "child lineage generation lost");
        check(group.spawned_by(grandchild) ==
                  std::make_optional(std::pair<std::uint64_t, std::uint64_t>{2, 1}),
              "grandchild lineage generation lost");
        check(group.mark_ready(parent) && group.mark_ready(child)
                && group.mark_ready(grandchild), "nested readiness failed");
        check(!group.finalized(parent), "parent retired while descendant still live");
        check(!group.finalized(child), "child retired while grandchild still live");
        check(parent.get_done().wait_for(0ms) == std::future_status::timeout,
              "parent done preceded descendants");
        check(group.finalized(grandchild), "grandchild retirement failed");
        check(child.get_done().wait_for(0ms) == std::future_status::timeout,
              "grandchild done notification fabricated parent done");
        check(group.finalized(child), "child retirement failed");
        check(!group.finalized(child), "duplicate child finalizer admitted");
        check(parent.get_done().wait_for(0ms) == std::future_status::timeout,
              "parent released while still live");
        check(group.finalized(parent), "parent physical finalization rejected");
        parent.get_done().get();
        check(group.worker_count() == 0, "native group retains completed subtree");
    }
    {
        NativeActorGroup group(6102, 4);
        std::atomic<int> stops{0};
        auto parent = group.adopt(6102, 1, 1, [&] { ++stops; });
        auto child = group.adopt_child(parent, 6102, 2, 1, [&] { ++stops; });
        auto grandchild = group.adopt_child(child, 6102, 3, 1, [&] { ++stops; });
        check(group.mark_ready(parent) && group.mark_ready(child)
              && group.mark_ready(grandchild), "nested startup failed");
        group.stop_subtree(parent);
        group.stop_subtree(parent);
        check(stops == 3, "subtree stop dropped or duplicated callback");
        bool rejected = false;
        try { (void) group.adopt_child(parent, 6102, 4, 1, [] {}); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "stopping parent admitted new child");
        check(!group.finalized(parent), "subtree stop fabricated parent done");
        check(group.finalized(grandchild), "grandchild not physically done");
        check(group.finalized(child), "child not physically done");
        check(group.finalized(parent), "parent not physically done");
        group.close();
        check(stops == 3, "group close sent duplicate stop");
    }
    {
        NativeActorGroup group(6103, 4);
        auto parent = group.adopt(6103, 1, 1, [] {});
        auto child = group.adopt_child(parent, 6103, 2, 4, [] {});
        check(group.mark_ready(parent) && group.mark_ready(child), "ready failed");
        auto pf = group.runtime_finalizer(parent);
        auto cf = group.runtime_finalizer(child);
        group.close();
        check(pf.ready_for_resource_reclamation(), "detached parent not fenced");
        check(!pf.finalized(), "detached parent retired before child");
        check(parent.get_done().wait_for(0ms) == std::future_status::timeout,
              "detached parent DONE fabricated");
        check(cf.finalized(), "detached child finalization failed");
        child.get_done().get();
        check(pf.finalized(), "detached parent did not join child finalizer");
        parent.get_done().get();
    }
    {
        NativeActorGroup primary(6104, 3), foreign(6104, 2);
        auto parent = primary.adopt(6104, 1, 1, [] {});
        auto outsider = foreign.adopt(6104, 9, 1, [] {});
        bool rejected = false;
        try { (void) primary.adopt_child(outsider, 6104, 2, 1, [] {}); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "foreign-group actor obtained child admission");
        auto child = primary.adopt_child(parent, 6104, 2, 1, [] {});
        check(!primary.finalized(parent), "parent retired with unjoined child");
        check(primary.finalized(child), "child did not retire");
        check(primary.finalized(parent), "parent did not retire");
        check(foreign.finalized(outsider), "outside actor did not retire");
    }
    {
        NativeActorGroup group(6105, 3);
        auto parent = group.adopt(6105, 1, 1, [] {});
        auto old_child = group.adopt_child(parent, 6105, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(old_child),
              "old generation failed readiness");
        check(group.finalized(old_child), "old generation not retired");
        auto replacement = group.adopt_child(parent, 6105, 2, 2, [] {});
        check(group.mark_ready(replacement), "new generation failed readiness");
        check(!group.finalized(old_child), "stale finalizer acknowledged new child");
        check(!group.finalized(parent),
              "late old-generation finalizer satisfied replacement join");
        check(group.spawned_by(replacement) ==
                std::make_optional(std::pair<std::uint64_t, std::uint64_t>{1, 1}),
              "replacement lost exact parent incarnation");
        check(group.finalized(replacement), "replacement did not retire");
        check(group.finalized(parent), "parent join did not release after replacement");
    }
    {
        // A nonparking finalizer retry can be armed after group detachment.
        // Completing the last child wakes it, but never publishes parent DONE
        // until the trusted runtime finalizer actually retries.
        NativeActorGroup group(6106, 3);
        auto parent = group.adopt(6106, 1, 1, [] {});
        auto child = group.adopt_child(parent, 6106, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(child),
              "child join notifier startup failed");
        auto parent_finalizer = group.runtime_finalizer(parent);
        auto child_finalizer = group.runtime_finalizer(child);
        group.close();
        check(!parent_finalizer.finalized(),
              "parent detached finalizer bypassed live child");
        std::atomic<int> wakeups{0};
        bool already_joined = group.register_children_drained(
            parent, [&] { wakeups.fetch_add(1); });
        check(!already_joined, "live child was reported as already drained");
        bool duplicate_denied = false;
        try {
            (void) group.register_children_drained(parent, [] {});
        } catch (const std::logic_error&) { duplicate_denied = true; }
        check(duplicate_denied, "duplicate join observer retained indefinitely");
        check(child_finalizer.finalized(), "child detached finalization failed");
        check(wakeups.load() == 1, "last child did not wake parent exactly once");
        check(parent.get_done().wait_for(0ms) == std::future_status::timeout,
              "child wakeup fabricated parent physical completion");
        check(parent_finalizer.finalized(), "parent finalizer retry rejected");
        parent.get_done().get();
        check(!parent_finalizer.finalized(), "duplicate parent finalization accepted");
    }
    {
        NativeActorGroup group(6107, 3);
        auto parent = group.adopt(6107, 1, 1, [] {});
        auto child = group.adopt_child(parent, 6107, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(child),
              "post-join registration startup failed");
        check(group.finalized(child), "last child physical finalization failed");
        std::atomic<int> wakeups{0};
        bool immediate = group.register_children_drained(
            parent, [&] { wakeups.fetch_add(1); });
        check(immediate && wakeups.load() == 0,
              "registering after drain must not manufacture asynchronous event");
        check(group.finalized(parent), "already-drained parent could not retire");
    }
    {
        // Last-child notification is a one-shot scheduler hint, not proof
        // that future nested admission is impossible. If a new child races
        // parent finalization, the owner counter must reject that finalizer
        // and permit a new observer to be armed.
        NativeActorGroup group(6108, 3);
        auto parent = group.adopt(6108, 1, 1, [] {});
        auto first = group.adopt_child(parent, 6108, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(first),
              "dynamic join startup failed");
        std::atomic<int> wakeups{0};
        check(!group.register_children_drained(parent, [&] { ++wakeups; }),
              "first child join was empty");
        check(group.finalized(first), "first child did not physically retire");
        check(wakeups == 1, "first child did not wake parent");
        auto second = group.adopt_child(parent, 6108, 3, 1, [] {});
        check(group.mark_ready(second), "second child did not start");
        check(!group.finalized(parent),
              "parent finalizer slipped through newly admitted child");
        check(!group.register_children_drained(parent, [&] { ++wakeups; }),
              "new child did not require rearming join observer");
        check(group.finalized(second), "second child did not retire");
        check(wakeups == 2, "rearmed last-child observer never fired");
        check(group.finalized(parent), "parent could not retire after rearmed join");
        parent.get_done().get();
    }
    {
        // A last-child scheduler wakeup that throws during enqueue must be
        // retained on the parent's generation-specific ticket for retry.
        NativeActorGroup group(9120, 3);
        auto parent = group.adopt(9120, 1, 1, [] {});
        auto child = group.adopt_child(parent, 9120, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(child),
              "failed-join startup failed");
        int wakes = 0;
        check(!group.register_children_drained(
            parent, [attempts = 0, &wakes]() mutable {
                if (attempts++ < 2)
                    throw std::runtime_error("join CONTROL enqueue rejected");
                ++wakes;
            }), "failed-join unexpectedly reported no children");
        check(group.finalized(child), "failed-join child did not retire");
        check(parent.child_join_notification_failed(),
              "failed last-child enqueue was discarded");
        check(!group.finalized(parent),
              "parent DONE bypassed undelivered child-drained notification");
        check(wakes == 0, "failed enqueue manufactured parent notification");
        check(parent.get_done().wait_for(0ms) == std::future_status::timeout,
              "failed join manufactured parent DONE");
        auto second = group.adopt_child(parent, 9120, 3, 1, [] {});
        check(group.mark_ready(second), "post-failed-join child not admitted");
        bool rejected = false;
        try { (void)group.register_children_drained(parent, [] {}); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "failed join notifier was silently overwritten");
        check(!NativeActorGroup::retry_failed_children_drained(parent),
              "second failed enqueue reported success");
        check(parent.child_join_notification_failed(),
              "second failed retry lost join receipt");
        check(NativeActorGroup::retry_failed_children_drained(parent),
              "join enqueue never recovered");
        check(!parent.child_join_notification_failed() && wakes == 1,
              "join retry was duplicated or failed to settle");
        check(!NativeActorGroup::retry_failed_children_drained(parent),
              "successful join retry admitted duplicate");
        check(!group.finalized(parent),
              "parent retired before newly admitted second child");
        check(!group.register_children_drained(parent, [&] { ++wakes; }),
              "parent could not rearm after recovering original join");
        check(group.finalized(second), "second child retirement failed");
        check(wakes == 2, "second child join notification missing");
        check(group.finalized(parent), "parent did not retire after both joins");
    }
    {
        // After group destruction, retained parent ticket and finalizer
        // still own the failed child-drained enqueue without UAF.
        std::optional<NativeActorRef> parent;
        std::optional<NativeActorGroup::RuntimeFinalizer> parent_finalizer;
        std::optional<NativeActorGroup::RuntimeFinalizer> child_finalizer;
        auto delivered = std::make_shared<std::atomic<int>>(0);
        {
            NativeActorGroup group(9121, 2);
            parent = group.adopt(9121, 1, 1, [] {});
            auto child = group.adopt_child(*parent, 9121, 2, 1, [] {});
            check(group.mark_ready(*parent) && group.mark_ready(child),
                  "detached failed-join startup failed");
            parent_finalizer = group.runtime_finalizer(*parent);
            child_finalizer = group.runtime_finalizer(child);
            check(!group.register_children_drained(
                *parent, [delivered, calls = 0]() mutable {
                    if (calls++ == 0)
                        throw std::runtime_error("detached wakeup enqueue failed");
                    ++*delivered;
                }), "detached join unexpectedly empty");
            group.close();
        }
        check(!parent_finalizer->finalized(), "parent detached before child");
        check(child_finalizer->finalized(), "detached child failed retirement");
        check(parent->child_join_notification_failed(),
              "detached child lost failed parent wakeup");
        check(!parent_finalizer->finalized(),
              "detached parent DONE bypassed failed join enqueue");
        check(NativeActorGroup::retry_failed_children_drained(*parent),
              "post-destruction parent wakeup not retryable");
        check(delivered->load() == 1 && !parent->child_join_notification_failed(),
              "detached notification not delivered exactly once");
        check(parent_finalizer->finalized(), "detached parent finalizer failed");
        parent->get_done().get();
    }
    // Concurrent last-child finalization versus observer registration has
    // exactly one legal outcome: synchronous already-drained, or one wakeup.
    {
        // Failing STOP admission is NOT delivery. It must remain retryable
        // without marking physical DONE, even after phase==stopping.
        NativeActorGroup group(9101, 3);
        auto attempts = std::make_shared<std::atomic<int>>(0);
        auto actor = group.adopt(9101, 1, 1, [attempts] {
            if (attempts->fetch_add(1) == 0)
                throw std::runtime_error("transient STOP enqueue rejection");
        });
        check(group.mark_ready(actor), "retry worker readiness failed");
        group.stop_workers();
        check(attempts->load() == 1, "initial STOP admission not attempted");
        check(actor.stop_delivery_failed(), "STOP failure was incorrectly acknowledged");
        check(actor.get_done().wait_for(0ms) == std::future_status::timeout,
              "failed STOP fabricated physical DONE");
        group.stop_workers();
        check(attempts->load() == 2, "STOP admission did not retry after failure");
        check(!actor.stop_delivery_failed(), "successful STOP retained failure receipt");
        group.stop_workers();
        check(attempts->load() == 2, "successful STOP callback issued twice");
        check(group.finalized(actor), "retry worker did not finalize");
    }
    {
        // The finalizer and trusted STOP retry token survive closing and
        // *destroying* their native manager. The callback capture itself is
        // heap-owned; the manager reference must never be captured here.
        std::optional<NativeActorRef> actor;
        std::optional<NativeActorGroup::RuntimeFinalizer> finalizer;
        auto attempts = std::make_shared<std::atomic<int>>(0);
        {
            NativeActorGroup group(9102, 2);
            actor = group.adopt(9102, 1, 1, [attempts] {
                if (attempts->fetch_add(1) == 0)
                    throw std::runtime_error("first STOP delivery rejected");
            });
            check(group.mark_ready(*actor), "detached retry startup failed");
            finalizer = group.runtime_finalizer(*actor);
            group.close();
            check(actor->stop_delivery_failed(), "detached STOP failure lost");
            check(attempts->load() == 1, "detached STOP not attempted");
        }
        check(actor->get_done().wait_for(0ms) == std::future_status::timeout,
              "detached STOP failure fabricated DONE");
        check(NativeActorGroup::retry_failed_stop(*actor),
              "retry after manager destruction did not deliver STOP");
        check(!actor->stop_delivery_failed(), "detached STOP failure not cleared");
        check(attempts->load() == 2, "detached STOP callback not retried exactly once");
        check(!NativeActorGroup::retry_failed_stop(*actor),
              "successful detached STOP allowed duplicate retry");
        check(finalizer->finalized(), "physically retired detached actor rejected");
        actor->get_done().get();
    }
    {
        // Retrying the complete subtree must reissue only the child whose
        // first STOP attempt failed; successful parent STOP is not duplicated.
        NativeActorGroup group(9103, 3);
        auto failures = std::make_shared<std::atomic<int>>(0);
        auto parent_stops = std::make_shared<std::atomic<int>>(0);
        auto parent = group.adopt(9103, 1, 1, [parent_stops] { ++*parent_stops; });
        auto child = group.adopt_child(parent, 9103, 2, 1, [failures] {
            if (failures->fetch_add(1) == 0)
                throw std::runtime_error("child STOP transient rejection");
        });
        check(group.mark_ready(parent) && group.mark_ready(child),
              "subtree retry readiness failed");
        group.stop_subtree(parent);
        check(parent_stops->load() == 1 && failures->load() == 1,
              "first subtree STOP callbacks not invoked");
        check(child.stop_delivery_failed(), "subtree child STOP failure lost");
        check(!group.finalized(parent), "parent reclaimed while child live");
        group.stop_subtree(parent);
        check(failures->load() == 2 && parent_stops->load() == 1,
              "subtree retry omitted child or duplicated successful parent");
        check(!child.stop_delivery_failed(), "subtree STOP failure did not clear");
        check(group.finalized(child), "subtree child finalization rejected");
        check(group.finalized(parent), "subtree parent join finalization rejected");
    }
    {
        // A mutable lambda captures its attempt counter *by value*. STOP
        // retry must invoke exactly the same callable object after close;
        // a copied pre-failure callback would throw forever.
        std::optional<NativeActorRef> detached;
        std::optional<NativeActorGroup::RuntimeFinalizer> finalize;
        {
            NativeActorGroup group(9104, 2);
            detached = group.adopt(9104, 1, 1, [attempts = 0]() mutable {
                if (attempts++ == 0)
                    throw std::runtime_error("mutable STOP callback rejected first enqueue");
            });
            finalize = group.runtime_finalizer(*detached);
            group.close();
            check(detached->stop_delivery_failed(), "mutable STOP failure not recorded");
        }
        check(NativeActorGroup::retry_failed_stop(*detached),
              "STOP retry lost mutable callback state across manager destruction");
        check(!detached->stop_delivery_failed(),
              "mutable callback successful retry not acknowledged");
        check(finalize->finalized(), "mutable callback actor could not finalize");
        detached->get_done().get();
    }
    for (int round = 0; round < 200; ++round) {
        const auto rid = static_cast<std::uint64_t>(8000 + round);
        NativeActorGroup group(rid, 2);
        auto parent = group.adopt(rid, 1, 1, [] {});
        auto child = group.adopt_child(parent, rid, 2, 1, [] {});
        check(group.mark_ready(parent) && group.mark_ready(child),
              "concurrent join actors did not start");
        std::atomic<int> wakes{0};
        std::atomic<bool> already{false};
        std::thread register_observer([&] {
            already = group.register_children_drained(
                parent, [&] { wakes.fetch_add(1); });
        });
        std::thread finalizer([&] {
            check(group.finalized(child), "racing child finalization rejected");
        });
        register_observer.join();
        finalizer.join();
        check((already && wakes == 0) || (!already && wakes == 1),
              "lost/duplicate parent child-drained notification");
        check(group.finalized(parent), "racing join parent failed finalization");
    }
    for (int round = 0; round < 100; ++round) {
        const auto rid = static_cast<std::uint64_t>(7000 + round);
        NativeActorGroup group(rid, 2);
        auto parent = group.adopt(rid, 1, 1, [] {});
        check(group.mark_ready(parent), "racing parent failed ready");
        std::optional<NativeActorRef> child;
        std::atomic<int> stops{0};
        std::atomic<bool> rejected{false};
        std::thread spawn([&] {
            try {
                child = group.adopt_child(parent, rid, 2, 1,
                                          [&] { stops.fetch_add(1); });
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        std::thread stop([&] { group.stop_subtree(parent); });
        spawn.join();
        stop.join();
        check((bool) child != rejected.load(), "spawn/STOP had no linearization");
        if (child) {
            check(stops == 1, "winning child escaped stop fence");
            check(!group.finalized(parent), "parent finalized before racing child");
            check(group.finalized(*child), "racing child finalization failed");
        }
        check(group.finalized(parent), "racing parent did not finalize");
        group.close();
        check(stops <= 1, "duplicate stop request after group close");
    }
}
