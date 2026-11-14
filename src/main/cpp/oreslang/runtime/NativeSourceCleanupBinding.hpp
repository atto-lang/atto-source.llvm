#pragma once

#include "oreslang/runtime/NativeContinuationStore.h"
#include "oreslang/runtime/NativeSourceCleanupGate.hpp"

#include <cstdint>

namespace oreslang::runtime {

// A staging, store-backed admission check for runtime-owned cleanup turns.
// This is NOT an authorization primitive exposed to guest code: the native
// scheduler must own the live store, handle and physical single-flight lease.
class NativeSourceCleanupBinding final {
public:
    NativeSourceCleanupBinding(OresNativeContinuationStore* store,
            OresNativeContinuationHandle handle,
            NativeSourceCleanupTicket ticket,
            std::uint64_t code_generation,
            std::uint64_t source_token) noexcept
        : store_(store), handle_(handle), ticket_(ticket),
          code_generation_(code_generation), source_token_(source_token) {}

    // Check a fresh generation-fenced, live CLAIMED continuation record
    // instead of trusting a naked (actor, generation, continuation) tuple.
    // actor_id names the allocation OWNER (actor slot), never the scheduler
    // runtime domain. The record must also prove that its allocation domain
    // is the same runtime domain that owns the claimed execution.
    // Caller MUST prevent store destruction and concurrent claim retirement
    // between verification and dispatch; a get() alone is not a carrier lease.
    bool claimed() const noexcept {
        if (!store_ || !ores_native_store_handle_valid(handle_) ||
                !ticket_.actor_id || !ticket_.actor_generation ||
                !code_generation_ || !source_token_ ||
                ticket_.continuation_id != ores_native_store_handle_pack(handle_))
            return false;
        OresNativeContinuationRecord record{};
        return ores_native_continuation_store_get(store_, handle_, &record) &&
            record.state == ORES_CONTINUATION_CLAIMED &&
            record.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR &&
            record.allocation.domain != 0 &&
            record.allocation.owner == ticket_.actor_id &&
            record.allocation.generation == ticket_.actor_generation &&
            record.execution_id.runtime_domain == record.allocation.domain &&
            record.code_generation == code_generation_ &&
            record.source_token == source_token_;
    }

    // A normal terminal result is also a positive physical reclamation
    // claim, and must not be minted from a caller-forgeable naked ticket.
    // The scheduler must first prove normal frame and FFI-loan teardown.
    bool complete_normally(NativeSourceCleanupGate& gate) const noexcept {
        return gate.belongs_to(ticket_.actor_id, ticket_.actor_generation) &&
            claimed() && gate.complete_normally(ticket_);
    }

    bool claim_cleanup(NativeSourceCleanupGate& gate) const noexcept {
        return gate.belongs_to(ticket_.actor_id, ticket_.actor_generation) &&
            claimed() && gate.try_claim_cleanup(ticket_);
    }

    bool yield_cleanup(NativeSourceCleanupGate& gate) const noexcept {
        return gate.belongs_to(ticket_.actor_id, ticket_.actor_generation) &&
            claimed() && gate.yield_cleanup(ticket_);
    }

    bool complete_cleanup(NativeSourceCleanupGate& gate) const noexcept {
        return gate.belongs_to(ticket_.actor_id, ticket_.actor_generation) &&
            claimed() && gate.complete_cleanup(ticket_);
    }

private:
    OresNativeContinuationStore* const store_;
    const OresNativeContinuationHandle handle_;
    const NativeSourceCleanupTicket ticket_;
    const std::uint64_t code_generation_;
    const std::uint64_t source_token_;
};

} // namespace oreslang::runtime
