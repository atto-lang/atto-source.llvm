#include "oreslang/runtime/NativeChildScope.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeChildScope;
using namespace std::chrono_literals;
static void check(bool yes, const char* error) {
    if (!yes) throw std::runtime_error(error);
}
int main() {
    {
        NativeActorGroup group(71, 3);
        auto parent = group.adopt(71, 1, 1, [] {});
        auto a = group.adopt(71, 2, 1, [] {});
        auto b = group.adopt(71, 3, 1, [] {});
        check(group.mark_ready(a) && group.mark_ready(b), "child ready failed");
        NativeChildScope scope(parent, 2);
        const auto ta = scope.reserve(), tb = scope.reserve();
        int stops_a = 0, stops_b = 0;
        scope.attach(ta, a, [&] { ++stops_a; });
        scope.attach(tb, b, [&] { ++stops_b; });
        scope.close(); scope.close();
        check(stops_a == 1 && stops_b == 1, "duplicate stop");
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "STOP fabricated done");
        check(group.finalized(a), "first child did not finalize");
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "parent scope finished before sibling finalization");
        check(group.finalized(b, "child failed"), "second child did not finalize");
        scope.get_done().get();
        check(scope.pending() == 0 && scope.failures() == 1,
              "terminal failure not reported and retired");
        group.close(); check(group.finalized(parent), "parent finalization failed");
    }
    {
        NativeActorGroup group(72, 2);
        auto parent = group.adopt(72, 1, 1, [] {});
        auto child = group.adopt(72, 2, 1, [] {});
        NativeChildScope scope(parent, 2);
        const auto abort_token = scope.reserve(), attach_token = scope.reserve();
        scope.close(); scope.abort(abort_token);
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "unresolved child reservation lost");
        int stops = 0;
        scope.attach(attach_token, child, [&] { ++stops; });
        check(stops == 1, "STOP lost in-flight attach");
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "child reservation converted to physical done");
        check(group.finalized(child), "post-STOP child did not finalize");
        scope.get_done().get();
        bool closed = false;
        try { (void) scope.reserve(); } catch (const std::logic_error&) { closed = true; }
        check(closed, "STOP reopened admission");
        group.close(); check(group.finalized(parent), "parent finalization failed");
    }
    {
        NativeActorGroup group(73, 2);
        auto parent = group.adopt(73, 1, 1, [] {});
        auto child = group.adopt(73, 2, 1, [] {});
        NativeChildScope scope(parent, 1);
        auto token = scope.reserve();
        scope.attach(token, child, [] {});
        bool duplicate = false;
        try { scope.attach(token, child, [] {}); }
        catch (const std::logic_error&) { duplicate = true; }
        check(duplicate, "duplicate token accepted");
        bool abort_rejected = false;
        try { scope.abort(token); }
        catch (const std::logic_error&) { abort_rejected = true; }
        check(abort_rejected, "published actor was abandoned by abort");
        scope.close();
        check(group.finalized(child), "child finalization failed");
        scope.get_done().get();
        group.close(); check(group.finalized(parent), "parent finalization failed");
    }
    {
        NativeActorGroup primary(74, 2), foreign(75, 1);
        auto parent = primary.adopt(74, 1, 1, [] {});
        auto child = primary.adopt(74, 2, 1, [] {});
        auto imposter = foreign.adopt(75, 2, 1, [] {});
        NativeChildScope scope(parent, 1);
        auto revoked = scope.reserve();
        scope.abort(revoked);
        auto token = scope.reserve();
        bool old_denied = false, foreign_denied = false;
        try { scope.attach(revoked, child, [] {}); }
        catch (const std::logic_error&) { old_denied = true; }
        try { scope.attach(token, imposter, [] {}); }
        catch (const std::invalid_argument&) { foreign_denied = true; }
        check(old_denied && foreign_denied,
              "stale reservation or foreign runtime passed child admission");
        scope.attach(token, child, [] {});
        scope.close();
        check(primary.finalized(child), "authorized child not finalized");
        scope.get_done().get();
        primary.close(); foreign.close();
        check(primary.finalized(parent), "parent not finalized");
        check(foreign.finalized(imposter), "foreign actor not finalized");
    }
    {
        NativeActorGroup group(76, 2);
        auto parent = group.adopt(76, 1, 1, [] {});
        auto child = group.adopt(76, 2, 1, [] {});
        NativeChildScope scope(parent, 1);
        scope.attach(scope.reserve(), child, [] {
            throw std::runtime_error("injected stop rejection");
        });
        scope.close();
        check(scope.stop_failures() == 1, "stop rejection not observable");
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "throwing stop forged child finalization");
        check(group.finalized(child), "stop failure blocked later finalization");
        scope.get_done().get();
        group.close(); check(group.finalized(parent), "parent not finalized");
    }
    for (int i = 0; i < 100; ++i) {
        auto runtime = static_cast<std::uint64_t>(100 + i);
        NativeActorGroup group(runtime, 2);
        auto parent = group.adopt(runtime, 1, 1, [] {});
        auto child = group.adopt(runtime, 2, 1, [] {});
        NativeChildScope scope(parent, 1);
        auto token = scope.reserve();
        std::atomic<int> stops{0};
        std::thread attach([&] {
            scope.attach(token, child, [&] { stops.fetch_add(1); });
        });
        std::thread close([&] { scope.close(); });
        attach.join(); close.join();
        check(stops == 1, "attach-vs-STOP duplicated or lost callback");
        check(scope.get_done().wait_for(0ms) == std::future_status::timeout,
              "attach-vs-STOP fabricated finalizer receipt");
        check(group.finalized(child), "racing child finalization failed");
        scope.get_done().get();
        group.close(); check(group.finalized(parent), "parent finalization failed");
    }
}
