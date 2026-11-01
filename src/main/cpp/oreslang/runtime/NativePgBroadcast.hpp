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
    if (max_recipients == 0) return {Report::Status::recipient_limit, 0, 0, 0, 0};
    auto snapshot = registry.snapshot_group(group_id);
    if (!snapshot) return {Report::Status::group_missing, 0, 0, 0, 0};
    if (snapshot->members.size() > max_recipients)
        return {Report::Status::recipient_limit, snapshot->members.size(), 0, 0, 0};
    Report report;
    report.snapshot_members = snapshot->members.size();
    for (const auto& member : snapshot->members) {
        // Closing/recreating a numeric group ID or retiring an actor cannot
        // authenticate the old member after the fact. A group membership may
        // still change immediately AFTER this check, so the mailbox gate is
        // independently authoritative.
        if (!registry.contains_snapshot_member(*snapshot, member)) {
            ++report.stale;
            continue;
        }
        if (send(member.actor_id, member.generation)) ++report.accepted;
        else ++report.rejected;
    }
    return report;
}

} // namespace oreslang::runtime
