#pragma once

#include "oreslang/runtime/RealtimeActorArena.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime::realtime {

// Trusted-runtime primitive, not a guest-visible pointer or a borrowed slice.
// Runs only inside the actor's active carrier turn and never returns raw storage.
// Operands are two LE signed-i64 cells in the actor's preallocated arena.
// Same-handle operands are allowed (including the exact same cell).
// No heap allocation, callback, buffer transfer, synchronization or throw.
enum class ColumnAddStatus : std::uint8_t {
    Ok,
    Denied,
    OutOfBounds,
    Overflow
};

inline std::uint64_t read_u64_le(const unsigned char* data) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= std::uint64_t(data[i]) << (8U * i);
    return value;
}

inline void write_u64_le(unsigned char* data, std::uint64_t value) noexcept {
    for (unsigned i = 0; i != 8; ++i)
        data[i] = static_cast<unsigned char>(value >> (8U * i));
}

// This function MUST remain in the trusted native runtime. No guest source may
// receive the pointer. Both cells are accessed without intermediary buffers:
// the arithmetic operates directly on actor-owned physical columns.
// The existing arena gate checks carrier TLS, epoch, owner, generation and
// retirement even for a zero-byte read. All remaining range checks are explicit.
inline ColumnAddStatus add_i64_cells(
    OresRealtimeActorArena* arena,
    OresRtArenaTurn turn,
    OresRtArenaHandle destination,
    std::uint64_t destination_offset,
    OresRtArenaHandle source,
    std::uint64_t source_offset) noexcept {
    if (!arena) return ColumnAddStatus::Denied;
    if (destination_offset > std::numeric_limits<std::size_t>::max() ||
        source_offset > std::numeric_limits<std::size_t>::max())
        return ColumnAddStatus::OutOfBounds;

    const auto dst_offset = static_cast<std::size_t>(destination_offset);
    const auto src_offset = static_cast<std::size_t>(source_offset);
    // Authority probe only: a zero-byte read at offset 0 runs the carrier,
    // epoch, owner, generation and retirement gate without the arena's span
    // check, so an out-of-range offset is reported as OutOfBounds below
    // rather than masked as Denied.
    if (ores_rt_actor_arena_read(arena, turn, destination, 0,
                                 nullptr, 0) != ORES_RT_ARENA_OK ||
        ores_rt_actor_arena_read(arena, turn, source, 0,
                                 nullptr, 0) != ORES_RT_ARENA_OK)
        return ColumnAddStatus::Denied;
    if ((dst_offset & 7U) != 0U || (src_offset & 7U) != 0U)
        return ColumnAddStatus::OutOfBounds;
    if (destination.index >= arena->slot_count ||
        source.index >= arena->slot_count)
        return ColumnAddStatus::Denied;

    const auto& dst_slot = arena->slots[static_cast<std::size_t>(destination.index)];
    const auto& src_slot = arena->slots[static_cast<std::size_t>(source.index)];
    if (dst_offset > dst_slot.length || sizeof(std::uint64_t) > dst_slot.length - dst_offset ||
        src_offset > src_slot.length || sizeof(std::uint64_t) > src_slot.length - src_offset)
        return ColumnAddStatus::OutOfBounds;

    const std::size_t dst_base =
        static_cast<std::size_t>(destination.index) * arena->slot_stride + dst_offset;
    const std::size_t src_base =
        static_cast<std::size_t>(source.index) * arena->slot_stride + src_offset;
    unsigned char* const dst = arena->slab + dst_base;
    const unsigned char* const src = arena->slab + src_base;
    const std::uint64_t lhs = read_u64_le(dst);
    const std::uint64_t rhs = read_u64_le(src);
    const std::uint64_t sum = lhs + rhs;
    // Signed two's-complement overflow, without undefined signed arithmetic.
    if (((~(lhs ^ rhs) & (lhs ^ sum)) & (std::uint64_t{1} << 63)) != 0)
        return ColumnAddStatus::Overflow;
    write_u64_le(dst, sum);
    return ColumnAddStatus::Ok;
}

} // namespace oreslang::runtime::realtime
