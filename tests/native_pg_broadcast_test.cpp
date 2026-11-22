#include "oreslang/runtime/NativePgBroadcast.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using Registry = oreslang::runtime::NativeActorMembershipRegistry;
using Report = oreslang::runtime::NativePgBroadcastReport;
using oreslang::runtime::native_pg_try_broadcast;

static void require(bool ok) {
    if (!ok) throw std::runtime_error("native pg broadcast check failed");
}

int main() {
    Registry m(42, 2, 2);
    auto join = m.create_group(7, 3);
    for (std::uint64_t i = 101; i <= 103; ++i) {
        require(m.register_actor(i, 1));
        require(m.join(join, i, 1));
    }

    std::size_t attempted = 0;
    auto accept = [&](std::uint64_t actor, std::uint64_t gen) noexcept {
        ++attempted;
        return actor >= 101 && actor <= 103 && gen == 1;
    };
    auto absent = native_pg_try_broadcast(m, 99, 3, accept);
    require(absent.status == Report::Status::group_missing && attempted == 0);
    auto zero = native_pg_try_broadcast(m, 7, 0, accept);
    require(zero.status == Report::Status::recipient_limit && attempted == 0);
    auto full = native_pg_try_broadcast(m, 7, 2, accept);
    require(full.status == Report::Status::recipient_limit &&
            full.snapshot_members == 0 && attempted == 0);
    auto too_small = m.snapshot_group_limited(7, 2);
    require(too_small.status == Registry::SnapshotStatus::recipient_limit &&
            !too_small.snapshot);
    auto not_found = m.snapshot_group_limited(99, 3);
    require(not_found.status == Registry::SnapshotStatus::group_missing &&
            !not_found.snapshot);
    auto bounded = m.snapshot_group_limited(7, 3);
    require(bounded.status == Registry::SnapshotStatus::ok &&
            bounded.snapshot && bounded.snapshot->members.size() == 3);
    auto complete = native_pg_try_broadcast(m, 7, 3, accept);
    require(complete.status == Report::Status::ok &&
            complete.snapshot_members == 3 && complete.accepted == 3 &&
            complete.rejected == 0 && complete.stale == 0 && attempted == 3);

    auto rejecting = native_pg_try_broadcast(m, 7, 3,
        [](std::uint64_t actor, std::uint64_t) noexcept {
            return actor != 102; // simulate a saturated recipient mailbox
        });
    require(rejecting.accepted == 2 && rejecting.rejected == 1 &&
            rejecting.stale == 0);

    auto previous = m.snapshot_group(7);
    require(previous.has_value() && previous->members.size() == 3);
    auto rotated = m.rotate_join(join);
    (void)rotated;
    // Rotating future join admission must NOT cancel existing pg membership.
    require(m.contains_snapshot_member(*previous, previous->members[0]));
    auto after_rotate = native_pg_try_broadcast(m, 7, 3, accept);
    require(after_rotate.accepted == 3);

    std::size_t entered = 0;
    auto closes_during_delivery = [&](std::uint64_t, std::uint64_t) noexcept {
        ++entered;
        if (entered == 1) (void)m.close_group(7);
        return true;
    };
    // Callback re-enters the same membership registry: no lock held in fanout.
    auto closed = native_pg_try_broadcast(m, 7, 3, closes_during_delivery);
    require(closed.accepted == 1 && closed.stale == 2 &&
            closed.rejected == 0 && entered == 1);
    require(!m.contains_snapshot_member(*previous, previous->members[0]));

    // Group-ID ABA: members may rejoin the new room with the SAME numeric ID,
    // but they must never be accepted against a snapshot of the old room.
    auto new_cap = m.create_group(7, 3);
    for (std::uint64_t i = 101; i <= 103; ++i)
        require(m.join(new_cap, i, 1));
    auto next = m.snapshot_group(7);
    require(next && next->group_incarnation != previous->group_incarnation);
    require(!m.contains_snapshot_member(*previous, previous->members[0]));
    require(native_pg_try_broadcast(m, 7, 3, accept).accepted == 3);

    require(m.retire_actor(101, 1));
    require(m.register_actor(101, 2));
    require(m.join(new_cap, 101, 2));
    require(!m.contains_snapshot_member(*next, next->members[0]));
    auto replacements = native_pg_try_broadcast(m, 7, 3,
        [](std::uint64_t actor, std::uint64_t gen) noexcept {
            return actor != 101 || gen == 2;
        });
    require(replacements.accepted == 3 && replacements.rejected == 0);

    Registry other(42, 1, 2);
    auto same_id = other.create_group(7, 3);
    require(other.register_actor(102, 1) && other.join(same_id, 102, 1));
    require(!other.contains_snapshot_member(*next, next->members[1]));

    // A churner may race snapshots. Each attempt is bounded, counts sum to
    // snapshot_members and never creates hidden retries or delivery threads.
    // Membership validation binds both group incarnation AND membership at
    // snapshot time. An actor joining later must not retroactively enter an
    // earlier fanout's recipient list.
    {
        Registry later(52, 1, 2);
        const auto room = later.create_group(8, 2);
        require(later.register_actor(501, 1) && later.register_actor(502, 1));
        require(later.join(room, 501, 1));
        const auto prior = later.snapshot_group_limited(8, 2);
        require(prior.status == Registry::SnapshotStatus::ok && prior.snapshot);
        require(later.join(room, 502, 1));
        const Registry::Member newcomer{502, 1};
        require(!later.contains_snapshot_member(*prior.snapshot, newcomer));
        const auto current = later.snapshot_group_limited(8, 2);
        require(current.snapshot && later.contains_snapshot_member(*current.snapshot, newcomer));
    }

    Registry concurrent(51, 1, 1);
    auto cjoin = concurrent.create_group(1, 4);
    for (std::uint64_t i = 1; i <= 4; ++i) require(concurrent.register_actor(i, 1));
    std::atomic<bool> go{false};
    std::thread worker([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
            if (!concurrent.join(cjoin, 1, 1)) std::terminate();
            if (!concurrent.leave(1, 1, 1)) std::terminate();
        }
    });
    go.store(true, std::memory_order_release);
    for (int i = 0; i < 2000; ++i) {
        const auto result = native_pg_try_broadcast(concurrent, 1, 4,
            [](std::uint64_t, std::uint64_t) noexcept { return true; });
        require(result.status == Report::Status::ok);
        require(result.accepted + result.rejected + result.stale ==
                result.snapshot_members);
        require(result.snapshot_members <= 1);
    }
    worker.join();
}
