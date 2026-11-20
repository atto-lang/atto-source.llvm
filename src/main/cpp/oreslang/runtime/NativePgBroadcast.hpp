#pragma once

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// Trusted-host, local-only pg-style best-effort delivery planner.
// This is not a distributed group protocol, a source guest API, or an atomic
// multi-recipient transaction. The callback MUST authenticate the member's
// real mailbox generation/domain and authorize admission before returning true.
struct NativePgBroadcastReport {
    enum class Status : std::uint8_t { ok, group_missing, recipient_limit };
    Status status = Status::ok;
    std::size_t snapshot_members = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t stale = 0;
};

// No callback runs with registry locks held. Overflow in max_recipients
// fails closed before *any* delivery; no unbounded per-recipient queue is
// constructed. Failed individual sends remain failed and are never retried
// implicitly, ensuring a slow recipient cannot grow hidden memory.
template<class Send>
NativePgBroadcastReport native_pg_try_broadcast(
        const NativeActorMembershipRegistry& registry,
        std::uint64_t group_id, std::size_t max_recipients, Send&& send) {
    static_assert(std::is_nothrow_invocable_r_v<bool, Send&,
                  std::uint64_t, std::uint64_t>,
                  "pg send adapter must be noexcept and return bool");
    using Report = NativePgBroadcastReport;
    // Budget the snapshot *inside* the registry lock, before allocation.
    // No unbounded group copy is allowed merely to discover an over-limit
    // fanout; this also avoids an unsynchronized count-then-copy race.
    auto selection = registry.snapshot_group_limited(group_id, max_recipients);
    if (selection.status == NativeActorMembershipRegistry::SnapshotStatus::group_missing)
        return {Report::Status::group_missing, 0, 0, 0, 0};
    if (selection.status == NativeActorMembershipRegistry::SnapshotStatus::recipient_limit)
        return {Report::Status::recipient_limit, 0, 0, 0, 0};
    const auto& snapshot = *selection.snapshot;
    Report report;
    report.snapshot_members = snapshot.members.size();
    for (const auto& member : snapshot.members) {
        // Closing/recreating a numeric group ID or retiring an actor cannot
        // authenticate the old member after the fact. A group membership may
        // still change immediately AFTER this check, so the mailbox gate is
        // independently authoritative.
        if (!registry.contains_snapshot_member(snapshot, member)) {
            ++report.stale;
            continue;
        }
        if (send(member.actor_id, member.generation)) ++report.accepted;
        else ++report.rejected;
    }
    return report;
}

} // namespace oreslang::runtime
