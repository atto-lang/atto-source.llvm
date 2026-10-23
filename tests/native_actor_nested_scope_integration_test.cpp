#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
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
