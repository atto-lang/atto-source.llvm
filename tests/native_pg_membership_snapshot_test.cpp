#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using Registry = oreslang::runtime::NativeActorMembershipRegistry;
static void require(bool ok) { if (!ok) throw std::runtime_error("pg snapshot proof failed"); }

int main() {
    Registry groups(50, 3, 2);
    auto room_a = groups.create_group(7, 6);
    auto room_b = groups.create_group(8, 6);
    require(groups.register_actor(100, 1));
    require(groups.register_actor(200, 1));
    require(groups.join(room_a, 100, 1));
    require(groups.join(room_b, 100, 1));
    require(groups.join(room_a, 200, 1));
    auto snap = groups.snapshot_group(7);
    require(snap.has_value() && snap->runtime_id == 50);
    require(snap->group_id == 7 && snap->members.size() == 2);
    require(snap->members[0].actor_id == 100 && snap->members[0].generation == 1);
    require(snap->members[1].actor_id == 200 && snap->members[1].generation == 1);
    require(!groups.snapshot_group(999).has_value());
    // A delivery callback may reenter leave/close: no registry lock is held
    // after the snapshot returns. The snapshot is historical, not authorization.
    for (const auto& recipient : snap->members)
        require(groups.leave(7, recipient.actor_id, recipient.generation));
    require(groups.member_count(7) == 0);
    require(groups.member_count(8) == 1);
    require(groups.join(room_a, 100, 1));
    require(groups.retire_actor(100, 1));
    require(!groups.contains(7, 100, 1) && !groups.contains(8, 100, 1));
    require(!groups.contains(7, snap->members[0].actor_id, snap->members[0].generation));
    require(groups.register_actor(100, 2));
    require(groups.join(room_a, 100, 2));
    require(!groups.contains(7, 100, 1));
    require(groups.close_group(7));
    auto fresh = groups.create_group(7, 6);
    auto newest = groups.snapshot_group(7);
    require(newest.has_value() && newest->members.empty());
    require(newest->join_epoch != snap->join_epoch);
    require(newest->registry_incarnation == snap->registry_incarnation);
    require(groups.join(fresh, 100, 2));
    Registry foreign(50, 1, 2);
    auto foreign_group = foreign.create_group(7, 2);
    require(foreign.register_actor(100, 2));
    require(foreign.join(foreign_group, 100, 2));
    require(foreign.snapshot_group(7)->registry_incarnation != newest->registry_incarnation);

    Registry concurrent(51, 1, 1);
    auto cap = concurrent.create_group(1, 8);
    std::atomic<bool> start{false};
    std::vector<std::thread> writers;
    for (std::uint64_t id = 1; id <= 8; ++id) {
        require(concurrent.register_actor(id, 1));
        writers.emplace_back([&, id] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < 500; ++i) {
                if (!concurrent.join(cap, id, 1)) std::terminate();
                if (!concurrent.leave(1, id, 1)) std::terminate();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 500; ++i) {
        auto snapshot = concurrent.snapshot_group(1);
        require(snapshot && snapshot->members.size() <= 8);
        for (const auto& member : snapshot->members)
            require(member.actor_id > 0 && member.actor_id <= 8 && member.generation == 1);
    }
    for (auto& writer : writers) writer.join();
    require(concurrent.member_count(1) == 0);
}
