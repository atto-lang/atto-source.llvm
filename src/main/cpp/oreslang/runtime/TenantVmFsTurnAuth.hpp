#pragma once
/*
 * Trusted host-only scheduler-turn filesystem authorization witness.
 *
 * This is NOT a guest ABI, capability token, or native sandbox. Only the
 * trusted scheduler may install a witness on its private Carrier. The host
 * checks it against live scheduler/actor state at every host-call entry.
 * It never accepts an identity, actor pointer or memory span from guest data.
 *
 * ABI-v5 filesystem dispatch remains disabled until reviewed signed-image
 * admission, compiler lowering and enforceable native confinement exist.
 */
#include <cstdint>

namespace oreslang::runtime::vm::internal {

struct FsTurnAuth final {
    std::uint64_t tenant_generation = 0;
    std::uint64_t actor_ref = 0; // slot zero/generation zero is a valid internal actor ref
    std::uint64_t actor_incarnation = 0; // changes on an actor restart, even if ref stays constant
    std::uint64_t turn_sequence = 0;     // odd only while this exact scheduler turn is active
    std::uint32_t actor_offset = 0;
    std::uint32_t actor_bytes = 0;
    std::uint32_t scratch_offset = 0;
    std::uint32_t scratch_bytes = 0;
};

inline bool valid_fs_turn(const FsTurnAuth& auth) noexcept {
    if (!auth.tenant_generation || !auth.actor_incarnation ||
        !auth.turn_sequence || !(auth.turn_sequence & 1u) ||
        !auth.actor_offset || !auth.actor_bytes || !auth.scratch_bytes) return false;
    const std::uint64_t actor_end = std::uint64_t{auth.actor_offset} + auth.actor_bytes;
    const std::uint64_t scratch_end = std::uint64_t{auth.scratch_offset} + auth.scratch_bytes;
    return actor_end <= (std::uint64_t{1} << 32) &&
           auth.scratch_offset >= auth.actor_offset &&
           scratch_end <= actor_end;
}

inline bool same_fs_turn(const FsTurnAuth& bound, const FsTurnAuth& live) noexcept {
    return valid_fs_turn(bound) && valid_fs_turn(live) &&
           bound.tenant_generation == live.tenant_generation &&
           bound.actor_ref == live.actor_ref &&
           bound.actor_incarnation == live.actor_incarnation &&
           bound.turn_sequence == live.turn_sequence &&
           bound.actor_offset == live.actor_offset &&
           bound.actor_bytes == live.actor_bytes &&
           bound.scratch_offset == live.scratch_offset &&
           bound.scratch_bytes == live.scratch_bytes;
}

/* Offset32 overflow is checked in 64 bits. Paths/payloads must be copied
 * from ACTOR-PRIVATE spans; read result publication is scratch-private.
 * Bounds validation is necessary but is NOT a substitute for guest CFI. */
inline bool actor_fs_span(const FsTurnAuth& auth, std::uint32_t offset,
                          std::uint32_t count) noexcept {
    if (!valid_fs_turn(auth)) return false;
    const std::uint64_t start = offset;
    const std::uint64_t end = start + count;
    return start >= auth.actor_offset &&
           end <= std::uint64_t{auth.actor_offset} + auth.actor_bytes;
}

inline bool scratch_fs_span(const FsTurnAuth& auth, std::uint32_t offset,
                            std::uint32_t count) noexcept {
    if (!actor_fs_span(auth, offset, count)) return false;
    const std::uint64_t start = offset;
    const std::uint64_t end = start + count;
    return start >= auth.scratch_offset &&
           end <= std::uint64_t{auth.scratch_offset} + auth.scratch_bytes;
}

/* No wraparound: zero is permanently unauthorized for an exhausted actor
 * incarnation. The restart/slot lifecycle must never resurrect stale I/O. */
inline std::uint64_t next_fs_incarnation(std::uint64_t previous) noexcept {
    return previous == 0 || previous == UINT64_MAX ? 0 : previous + 1;
}

} // namespace oreslang::runtime::vm::internal
