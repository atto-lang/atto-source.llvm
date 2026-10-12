#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <limits>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorGroup;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

template<class Fn>
static bool rejected(Fn fn) {
    try { fn(); } catch (const std::logic_error&) { return true; }
    return false;
}

int main() {
    {
        // Regression: the registry-origin fence is not permitted to wrap
        // back to a previously issued identifier after reaching UINT64_MAX.
        std::atomic<std::uint64_t> ids{
                std::numeric_limits<std::uint64_t>::max() - 1};
        check(oreslang::runtime::detail::take_nonwrapping_registry_id(ids)
                  == std::numeric_limits<std::uint64_t>::max() - 1,
              "failed to issue final safe registry incarnation ID");
        bool exhausted = false;
        try {
            (void)oreslang::runtime::detail::take_nonwrapping_registry_id(ids);
        } catch (const std::overflow_error&) {
            exhausted = true;
        }
        check(exhausted, "registry incarnation identifiers wrapped at exhaustion");
        check(ids.load() == std::numeric_limits<std::uint64_t>::max(),
              "exhausted registry ID source mutated or wrapped");
    }
    {
        NativeActorMembershipRegistry m(10, 8, 2);
        auto a = m.create_group(1, 3);
        auto b = m.create_group(2, 3);
        auto c = m.create_group(3, 3);
        check(m.register_actor(100, 7), "register live connection actor");
        check(m.join(a, 100, 7), "join A");
        check(m.join(b, 100, 7), "join B");
        check(!m.join(a, 100, 7), "duplicate membership charged twice");
        check(m.member_count(1) == 1 && m.membership_count(100, 7) == 2,
              "dual membership indexes inconsistent");
        check(rejected([&] { (void)m.join(c, 100, 7); }), "actor quota bypass");
        check(m.leave(1, 100, 7), "leave A");
        check(!m.contains(1, 100, 7) && m.contains(2, 100, 7),
              "leaving A revoked B");
        check(m.join(c, 100, 7), "leaving A did not reclaim actor quota");
        check(m.close_group(2), "close B");
        check(m.membership_count(100, 7) == 1 && m.contains(3, 100, 7),
              "closing B removed C membership or leaked quota");
        check(m.join(a, 100, 7), "room B closure did not free membership");
        check(m.retire_actor(100, 7), "retire actor");
        check(m.member_count(1) == 0 && m.member_count(3) == 0,
              "retirement leaked memberships");
        check(!m.retire_actor(100, 7), "duplicate retirement succeeded");
        check(m.register_actor(100, 8), "register replacement");
        check(!m.leave(1, 100, 7), "stale actor removed replacement membership");
        check(rejected([&] { (void)m.join(a, 100, 7); }),
              "stale actor joined with old incarnation");
        check(m.join(a, 100, 8), "replacement cannot join");
        check(!m.retire_actor(100, 7) && m.contains(1, 100, 8),
              "old generation retired replacement");
        check(m.retire_actor(100, 8), "retire replacement");
    }
    {
        NativeActorMembershipRegistry m(20, 2, 3);
        auto a = m.create_group(1, 1);
        check(m.register_actor(1, 1) && m.register_actor(2, 1),
              "actor registration failed");
        check(rejected([&] { (void)m.register_actor(1, 2); }),
              "reused ID escaped live actor registry");
        check(m.join(a, 1, 1), "first admission failed");
        check(rejected([&] { (void)m.join(a, 2, 1); }),
              "group membership quota exceeded");
        auto fresh = m.rotate_join(a);
        check(rejected([&] { (void)m.join(a, 2, 1); }),
              "revoked permit remained live");
        check(m.leave(1, 1, 1), "leave failed");
        check(m.join(fresh, 2, 1), "new permit not accepted");
        check(m.close_group(1), "close failed");
        auto recreated = m.create_group(1, 2);
        check(rejected([&] { (void)m.join(fresh, 1, 1); }),
              "ABA: capability for closed group authorized replacement");
        check(m.join(recreated, 1, 1), "new group permit rejected");
        check(rejected([&] { (void)m.create_group(1, 2); }),
              "duplicate group accepted");
        auto other = m.create_group(2, 1);
        check(rejected([&] { (void)m.create_group(3, 1); }),
              "registry group quota bypass");
        NativeActorMembershipRegistry foreign(21, 2, 3);
        check(foreign.register_actor(1, 1), "foreign actor registration");
        bool foreign_rejected = false;
        try { (void)foreign.join(recreated, 1, 1); }
        catch (const std::invalid_argument&) { foreign_rejected = true; }
        check(foreign_rejected, "cross-runtime group permit admitted");
        check(m.close_group(2), "second group failed to close");
        (void)other;
    }
    {
        // Real lifecycle owner remains unique, independent of multiple group
        // memberships. Closing either cooperation group does not stop worker.
        NativeActorMembershipRegistry m(31, 4, 3);
        NativeActorGroup owner(31, 1), competing(31, 1);
        auto ref = owner.adopt(31, 77, 11, [] {});
        check(owner.mark_ready(ref), "owned actor failed readiness");
        check(m.register_actor(77, 11), "register owned actor");
        auto a = m.create_group(10, 4);
        auto b = m.create_group(20, 4);
        check(m.join(a, 77, 11) && m.join(b, 77, 11), "join two room groups");
        check(rejected([&] { (void)competing.adopt(31, 77, 11, [] {}); }),
              "second lifecycle owner adopted multi-group actor");
        check(m.close_group(10) && m.contains(20, 77, 11),
              "closing one room changed other room");
        check(ref.get_done().wait_for(std::chrono::milliseconds(0))
                  == std::future_status::timeout,
              "room close fabricated actor finalization");
        check(m.retire_actor(77, 11), "retire membership on actor teardown");
        check(owner.finalized(ref), "lifecycle owner did not finalize");
        ref.get_done().get();
    }
    {
        // Race: two concurrent joins must never evade the actor quota. A
        // group close racing joins must never resurrect closed membership.
        for (int round = 0; round < 250; ++round) {
            NativeActorMembershipRegistry m(40, 3, 1);
            auto a = m.create_group(1, 2), b = m.create_group(2, 2);
            check(m.register_actor(5, 1), "race actor register");
            std::atomic<int> admitted{0};
            std::thread t1([&] { try { if (m.join(a, 5, 1)) ++admitted; } catch (const std::logic_error&) {} });
            std::thread t2([&] { try { if (m.join(b, 5, 1)) ++admitted; } catch (const std::logic_error&) {} });
            t1.join(); t2.join();
            check(admitted == 1 && m.membership_count(5, 1) == 1,
                  "concurrent joins exceeded quota");
            std::thread closer([&] { (void)m.close_group(1); });
            std::thread leaver([&] { (void)m.leave(1, 5, 1); });
            closer.join(); leaver.join();
            check(!m.contains(1, 5, 1), "closed group resurrected member");
            check(m.retire_actor(5, 1), "race actor retire");
            check(m.member_count(2) == 0, "retirement left group members");
        }
    }
    {
        // Rotation, join, and duplicate admission share one serialization
        // point. Even if the old permit won an earlier join, its replay after
        // rotation must fail rather than silently return "already joined".
        for (int round = 0; round < 128; ++round) {
            NativeActorMembershipRegistry m(50, 2, 2);
            auto stale = m.create_group(1, 2);
            check(m.register_actor(100, 9), "race registration failed");
            std::atomic<bool> go{false};
            std::atomic<int> admitted{0};
            std::atomic<int> revoked{0};
            std::atomic<int> unexpected{0};
            std::thread joiner([&] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                try {
                    if (m.join(stale, 100, 9)) ++admitted;
                } catch (const std::logic_error&) {
                    ++revoked;
                } catch (...) {
                    ++unexpected;
                }
            });
            go.store(true, std::memory_order_release);
            auto fresh = m.rotate_join(stale);
            joiner.join();
            check(unexpected == 0 && admitted + revoked == 1,
                  "rotation/join was not linearizable");
            check(rejected([&] { (void)m.join(stale, 100, 9); }),
                  "stale capability replay succeeded after rotation");
            check(m.join(fresh, 100, 9) == (admitted == 0),
                  "fresh admission inconsistent with earlier race winner");
            check(!m.join(fresh, 100, 9),
                  "fresh duplicate admission charged membership twice");
            check(m.member_count(1) == 1 && m.membership_count(100, 9) == 1,
                  "rotation race corrupted forward or reverse membership");
            check(m.retire_actor(100, 9), "rotation race actor retirement failed");
        }
    }
    {
        // Numeric runtime IDs are not capability origins. Two registries for
        // the same trusted runtime ID must still reject one another's permits.
        NativeActorMembershipRegistry first(60, 1, 1);
        NativeActorMembershipRegistry second(60, 1, 1);
        auto permit = first.create_group(1, 1);
        auto local = second.create_group(1, 1);
        check(second.register_actor(55, 2), "second registry actor registration");
        bool foreign_rejected = false;
        try { (void)second.join(permit, 55, 2); }
        catch (const std::invalid_argument&) { foreign_rejected = true; }
        check(foreign_rejected, "same-runtime-ID foreign registry permit was accepted");
        check(second.join(local, 55, 2), "local permit rejected after foreign probe");
    }
    {
        // A manager closing the room competes with permit rotation on the
        // same registry lock. Neither a newly issued nor an old permit can
        // bring a closed room back, even when its numeric ID gets reused.
        for (int round = 0; round < 96; ++round) {
            NativeActorMembershipRegistry m(70, 2, 2);
            auto old = m.create_group(1, 2);
            check(m.register_actor(88, 3), "close/rotate actor registration failed");
            std::atomic<bool> go{false};
            std::vector<NativeActorMembershipRegistry::JoinCapability> issued;
            std::atomic<bool> unexpected{false};
            std::thread rotator([&] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                try {
                    auto fresh = m.rotate_join(old);
                    issued.push_back(fresh);
                    // A concurrent close can win before this admission.
                    try { (void)m.join(fresh, 88, 3); }
                    catch (const std::logic_error&) { }
                } catch (const std::logic_error&) {
                    // The close linearized before rotation.
                } catch (...) {
                    unexpected.store(true, std::memory_order_relaxed);
                }
            });
            go.store(true, std::memory_order_release);
            check(m.close_group(1), "close/rotate race failed to close group");
            rotator.join();
            check(!unexpected.load() && m.member_count(1) == 0
                      && m.membership_count(88, 3) == 0,
                  "close/rotate resurrected membership or corrupted indexes");
            check(rejected([&] { (void)m.join(old, 88, 3); }),
                  "old capability admitted closed room after close/rotate");
            for (const auto& rotatedPermit : issued) {
                check(rejected([&] { (void)m.join(rotatedPermit, 88, 3); }),
                      "rotated capability admitted closed room");
            }
            auto successor = m.create_group(1, 2);
            check(rejected([&] { (void)m.join(old, 88, 3); }),
                  "old capability admitted recreated room after close/rotate");
            for (const auto& rotatedPermit : issued) {
                check(rejected([&] { (void)m.join(rotatedPermit, 88, 3); }),
                      "rotated capability admitted recreated room");
            }
            check(m.join(successor, 88, 3),
                  "new incarnation capability rejected after close/rotate");
        }
    }
}
