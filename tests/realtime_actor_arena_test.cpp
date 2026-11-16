#include "oreslang/runtime/RealtimeActorArena.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    constexpr std::size_t kStorageBytes = 64 * 1024;
    alignas(std::max_align_t) std::array<unsigned char, kStorageBytes> storage{};

    const std::size_t required =
        ores_rt_actor_arena_required_bytes(4, 64, 4096);
    require(required > 0 && required <= storage.size(),
            "required byte calculation");

    OresRealtimeActorArena arena{};
    require(ores_rt_actor_arena_init(
                &arena, storage.data(), storage.size(),
                77, 11, 3, 4, 64, 4096) == ORES_RT_ARENA_OK,
            "arena init");

    OresRtArenaTurn turn{};
    require(ores_rt_actor_arena_enter(&arena, 11, 3, &turn) ==
                ORES_RT_ARENA_NOT_SEALED,
            "unsealed arena must reject entry");
    require(ores_rt_actor_arena_seal(&arena, 4096) == ORES_RT_ARENA_OK,
            "arena seal");
    // Output capabilities are writable memory too. Reject pointers into the
    // private backing or the arena's own identity/control header before
    // incrementing the physical-turn epoch.
    require(ores_rt_actor_arena_enter(
                &arena, 11, 3,
                reinterpret_cast<OresRtArenaTurn*>(arena.slab)) ==
                ORES_RT_ARENA_INVALID,
            "turn token cannot be written into private slab");
    require(ores_rt_actor_arena_enter(
                &arena, 11, 3,
                reinterpret_cast<OresRtArenaTurn*>(&arena)) ==
                ORES_RT_ARENA_INVALID,
            "turn token cannot overwrite arena identity");
    auto* invalid_turn_out = reinterpret_cast<OresRtArenaTurn*>(
        std::numeric_limits<std::uintptr_t>::max() - 3);
    require(ores_rt_actor_arena_enter(
                &arena, 11, 3, invalid_turn_out) == ORES_RT_ARENA_INVALID,
            "overflowing turn output pointer must reject before epoch");
    require(arena.epoch == 0 && !arena.active,
            "forged output tokens cannot advance turn epoch");
    require(ores_rt_actor_arena_enter(&arena, 99, 3, &turn) ==
                ORES_RT_ARENA_WRONG_OWNER,
            "wrong owner");
    require(ores_rt_actor_arena_enter(&arena, 11, 3, &turn) ==
                ORES_RT_ARENA_OK,
            "enter");

    OresRtArenaTurn second{};
    require(ores_rt_actor_arena_enter(&arena, 11, 3, &second) ==
                ORES_RT_ARENA_BUSY,
            "double enter");

    const auto initial_free_count = arena.free_count;
    require(ores_rt_actor_arena_alloc(
                &arena, turn, 16,
                reinterpret_cast<OresRtArenaHandle*>(arena.slab)) ==
                ORES_RT_ARENA_INVALID,
            "allocation token cannot overwrite actor slab");
    require(ores_rt_actor_arena_alloc(
                &arena, turn, 16,
                reinterpret_cast<OresRtArenaHandle*>(&arena)) ==
                ORES_RT_ARENA_INVALID,
            "allocation token cannot overwrite actor control");
    auto* invalid_handle_out = reinterpret_cast<OresRtArenaHandle*>(
        std::numeric_limits<std::uintptr_t>::max() - 3);
    require(ores_rt_actor_arena_alloc(
                &arena, turn, 16, invalid_handle_out) == ORES_RT_ARENA_INVALID,
            "overflowing handle output pointer must reject");
    require(arena.free_count == initial_free_count,
            "rejected allocation output cannot consume free-list slot");

    const auto initial_scratch_top = arena.scratch_top;
    require(ores_rt_actor_arena_scratch_alloc(
                &arena, turn, 16, alignof(std::uint64_t),
                reinterpret_cast<OresRtArenaScratch*>(arena.scratch)) ==
                ORES_RT_ARENA_INVALID,
            "scratch token cannot overwrite private scratch");
    require(ores_rt_actor_arena_scratch_alloc(
                &arena, turn, 16, alignof(std::uint64_t),
                reinterpret_cast<OresRtArenaScratch*>(&arena)) ==
                ORES_RT_ARENA_INVALID,
            "scratch token cannot overwrite actor control");
    auto* invalid_scratch_out = reinterpret_cast<OresRtArenaScratch*>(
        std::numeric_limits<std::uintptr_t>::max() - 3);
    require(ores_rt_actor_arena_scratch_alloc(
                &arena, turn, 16, alignof(std::uint64_t),
                invalid_scratch_out) == ORES_RT_ARENA_INVALID,
            "overflowing scratch output pointer must reject");
    require(arena.scratch_top == initial_scratch_top,
            "rejected scratch output cannot consume capacity");

    std::array<OresRtArenaHandle, 4> handles{};
    for (auto& handle : handles) {
        require(ores_rt_actor_arena_alloc(&arena, turn, 16, &handle) ==
                    ORES_RT_ARENA_OK,
                "slot allocation");
    }
    OresRtArenaHandle overflow{};
    require(ores_rt_actor_arena_alloc(&arena, turn, 8, &overflow) ==
                ORES_RT_ARENA_CAPACITY,
            "fixed capacity exhaustion");

    const std::uint64_t value = 0x1122334455667788ULL;
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, &value, sizeof(value)) ==
                ORES_RT_ARENA_OK,
            "persistent write");
    std::uint64_t observed = 0;
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK,
            "persistent read");
    require(observed == value, "persistent value");
    // Reinitialization while the physical carrier owns a turn would erase
    // the allocator, epoch and slot data. It must reject before touching any
    // bytes, even when a new owner/domain is supplied.
    const std::uint64_t active_epoch = arena.epoch;
    const std::size_t active_free_count = arena.free_count;
    require(ores_rt_actor_arena_init(
                &arena, storage.data(), storage.size(),
                88, 11, 3, 4, 64, 4096) == ORES_RT_ARENA_BUSY,
            "active carrier cannot reinitialize its arena");
    require(arena.domain == 77 && arena.epoch == active_epoch &&
                arena.free_count == active_free_count && arena.active,
            "rejected reinit preserves owner epoch and allocation capacity");
    observed = 0;
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "failed active reinit preserves the live allocation and carrier");


    // A stale or malicious host adapter must not be able to turn an arena
    // data-copy into corruption of private slot, scratch or control metadata.
    // All these failures must preserve the live actor's valid turn.
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, arena.slab, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "read destination inside private backing must reject");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, arena.slab, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "write source inside private backing must reject");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &arena, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "read destination overlapping control metadata must reject");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, &arena, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "write source overlapping control metadata must reject");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, nullptr, 0) ==
                ORES_RT_ARENA_OK,
            "zero-byte read remains a valid no-op");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, nullptr, 0) ==
                ORES_RT_ARENA_OK,
            "zero-byte write remains a valid no-op");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, storage.data() + required, 1) ==
                ORES_RT_ARENA_OK,
            "byte immediately beyond private backing remains accessible");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "rejected aliases cannot change live slot data or turn");

    // A span touching the final private byte must reject even if its other
    // byte is in spare caller storage. This catches containment-only checks
    // that would allow partially overlapping buffers across a region edge.
    auto* private_end = storage.data() + required;
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, private_end - 1, 2) ==
                ORES_RT_ARENA_INVALID,
            "partial read overlap at arena high boundary must reject");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, private_end - 1, 2) ==
                ORES_RT_ARENA_INVALID,
            "partial write overlap at arena high boundary must reject");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, private_end, 1) ==
                ORES_RT_ARENA_OK,
            "exact arena high boundary must remain disjoint");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, private_end, 1) ==
                ORES_RT_ARENA_OK,
            "external byte at exact boundary must be writable source");
    // A control-object suffix intersecting by one byte is equally forbidden.
    auto* control_end = reinterpret_cast<unsigned char*>(&arena) + sizeof(arena);
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, control_end - 1, 2) ==
                ORES_RT_ARENA_INVALID,
            "partial read overlap at arena control boundary must reject");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, control_end - 1, 2) ==
                ORES_RT_ARENA_INVALID,
            "partial write overlap at arena control boundary must reject");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "overlap denial must not modify persistent bytes");
    OresRtArenaHandle forged = handles[0];
    ++forged.domain;
    require(ores_rt_actor_arena_read(
                &arena, turn, forged, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_STALE,
            "forged domain must fail before touching external output");
    forged = handles[0];
    ++forged.generation;
    require(ores_rt_actor_arena_write(
                &arena, turn, forged, 0, &value, sizeof(value)) ==
                ORES_RT_ARENA_STALE,
            "forged generation must fail before touching private bytes");

    // A forged nonempty host span whose uintptr_t end wraps must fail
    // before any attempt to dereference the address.
    auto* wrapping = reinterpret_cast<unsigned char*>(
        std::numeric_limits<std::uintptr_t>::max() - 3);
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, wrapping, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "overflowing read destination range must reject");
    require(ores_rt_actor_arena_write(
                &arena, turn, handles[0], 0, wrapping, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "overflowing write source range must reject");
    require(ores_rt_actor_arena_read(
                &arena, turn, handles[0], 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "wrapped host spans cannot modify persistent allocation");

    const OresRtArenaHandle stale = handles[0];
    require(ores_rt_actor_arena_drop(&arena, turn, handles[0]) ==
                ORES_RT_ARENA_OK,
            "drop");
    require(ores_rt_actor_arena_read(
                &arena, turn, stale, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_STALE,
            "stale generation rejected");

    OresRtArenaHandle reused{};
    require(ores_rt_actor_arena_alloc(&arena, turn, 16, &reused) ==
                ORES_RT_ARENA_OK,
            "slot reuse");
    require(reused.index == stale.index &&
                reused.generation == stale.generation + 1,
            "generation increments on reuse");
    observed = std::numeric_limits<std::uint64_t>::max();
    require(ores_rt_actor_arena_read(
                &arena, turn, reused, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK,
            "reused read");
    require(observed == 0, "dropped storage must be zeroized");

    OresRtArenaScratch scratch{};
    require(ores_rt_actor_arena_scratch_alloc(
                &arena, turn, 32, alignof(std::uint64_t), &scratch) ==
                ORES_RT_ARENA_OK,
            "scratch allocation");
    require((scratch.offset % alignof(std::uint64_t)) == 0,
            "scratch alignment");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch, 0, &value, sizeof(value)) ==
                ORES_RT_ARENA_OK,
            "scratch write");
    observed = 0;
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK,
            "scratch read");
    require(observed == value, "scratch value");
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, arena.scratch, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch read cannot alias private backing");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch, 0, arena.scratch, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch write cannot alias private backing");
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, &arena, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch read cannot overwrite control metadata");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch, 0, &arena, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch write cannot read control metadata");
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "rejected scratch aliases preserve valid data");
    // Scratch is subject to the same alias and capability boundaries.
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0,
                storage.data() + required - 1, 2) == ORES_RT_ARENA_INVALID,
            "scratch read must reject partial arena-end overlap");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch, 0,
                storage.data() + required - 1, 2) == ORES_RT_ARENA_INVALID,
            "scratch write must reject partial arena-end overlap");
    OresRtArenaScratch stale_scratch = scratch;
    ++stale_scratch.epoch;
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, stale_scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_STALE,
            "scratch epoch forgery must reject");
    stale_scratch = scratch;
    ++stale_scratch.domain;
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, stale_scratch, 0, &value, sizeof(value)) ==
                ORES_RT_ARENA_STALE,
            "scratch domain forgery must reject");
    // Impossible offset/length tokens must reject prior to any pointer
    // arithmetic or external copy, even when domain and epoch are genuine.
    stale_scratch = scratch;
    stale_scratch.offset = std::numeric_limits<std::uint64_t>::max();
    stale_scratch.length = std::numeric_limits<std::uint64_t>::max();
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, stale_scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_STALE,
            "overflow-sized forged scratch extent cannot be read");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, stale_scratch, 0, &value, sizeof(value)) ==
                ORES_RT_ARENA_STALE,
            "overflow-sized forged scratch extent cannot be written");
    stale_scratch = scratch;
    stale_scratch.length = std::numeric_limits<std::uint64_t>::max();
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, stale_scratch, 0, &observed, 1) ==
                ORES_RT_ARENA_STALE,
            "in-bounds scratch start with oversized length must reject");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch,
                std::numeric_limits<std::size_t>::max(), &value, 1) ==
                ORES_RT_ARENA_INVALID,
            "huge per-scratch write offset must reject without wrapping");
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch,
                std::numeric_limits<std::size_t>::max(), &observed, 1) ==
                ORES_RT_ARENA_INVALID,
            "huge per-scratch read offset must reject without wrapping");
    observed = 0;
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_OK && observed == value,
            "forged scratch references leave legitimate data unchanged");
    require(ores_rt_actor_arena_scratch_read(
                &arena, turn, scratch, 0, wrapping, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch read rejects wrapping output");
    require(ores_rt_actor_arena_scratch_write(
                &arena, turn, scratch, 0, wrapping, sizeof(value)) ==
                ORES_RT_ARENA_INVALID,
            "scratch write rejects wrapping input");

    require(ores_rt_actor_arena_retire(&arena) == ORES_RT_ARENA_BUSY,
            "active arena cannot retire");
    require(ores_rt_actor_arena_leave(&arena, turn) == ORES_RT_ARENA_OK,
            "leave");

    OresRtArenaTurn next{};
    require(ores_rt_actor_arena_enter(&arena, 11, 3, &next) ==
                ORES_RT_ARENA_OK,
            "next turn");
    require(ores_rt_actor_arena_scratch_read(
                &arena, next, scratch, 0, &observed, sizeof(observed)) ==
                ORES_RT_ARENA_STALE,
            "scratch cannot cross turns");
    require(ores_rt_actor_arena_leave(&arena, next) == ORES_RT_ARENA_OK,
            "second leave");

    require(ores_rt_actor_arena_retire(&arena) == ORES_RT_ARENA_OK,
            "retire");
    require(ores_rt_actor_arena_enter(&arena, 11, 3, &next) ==
                ORES_RT_ARENA_RETIRED,
            "retired arena cannot reenter");

    require(ores_rt_actor_arena_required_bytes(
                std::numeric_limits<std::size_t>::max(), 2, 1) == 0,
            "dimension overflow must reject");

    alignas(std::max_align_t) std::array<unsigned char, 1024> tiny{};
    OresRealtimeActorArena tiny_arena{};
    require(ores_rt_actor_arena_init(
                &tiny_arena, tiny.data() + 1, tiny.size() - 1,
                1, 1, 1, 1, 8, 8) == ORES_RT_ARENA_ALIGNMENT,
            "misaligned backing must reject");

    alignas(std::max_align_t)
        std::array<unsigned char, sizeof(OresRealtimeActorArena) + 1024> overlap{};
    auto* overlapping_control =
        reinterpret_cast<OresRealtimeActorArena*>(overlap.data());
    require(ores_rt_actor_arena_init(
                overlapping_control,
                overlap.data(),
                overlap.size(),
                2, 2, 2, 1, 8, 8) == ORES_RT_ARENA_INVALID,
            "control object cannot overlap backing memory");

    alignas(std::max_align_t) std::array<unsigned char, 4096> odd_slots{};
    OresRealtimeActorArena aligned_arena{};
    require(ores_rt_actor_arena_init(
                &aligned_arena, odd_slots.data(), odd_slots.size(),
                3, 3, 3, 3, 17, 64) == ORES_RT_ARENA_OK,
            "odd logical slot size must use aligned physical stride");
    require(aligned_arena.slot_stride >= aligned_arena.slot_bytes,
            "slot stride must cover logical bytes");
    require((aligned_arena.slot_stride % alignof(std::max_align_t)) == 0,
            "slot stride must preserve max alignment");
    for (std::size_t i = 0; i < aligned_arena.slot_count; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(
            aligned_arena.slab + i * aligned_arena.slot_stride);
        require((address % alignof(std::max_align_t)) == 0,
                "every physical slot start must be max aligned");
    }
    require(ores_rt_actor_arena_seal(&aligned_arena, 1u << 20) ==
                ORES_RT_ARENA_OK,
            "prefault step larger than arena must terminate safely");

    // Failed allocation validation must not consume the free-stack entry.
    alignas(std::max_align_t) std::array<unsigned char, 4096> corrupt_slots{};
    OresRealtimeActorArena corrupt_arena{};
    require(ores_rt_actor_arena_init(
                &corrupt_arena, corrupt_slots.data(), corrupt_slots.size(),
                4, 4, 4, 2, 16, 64) == ORES_RT_ARENA_OK,
            "corrupt-free-stack arena init");
    require(ores_rt_actor_arena_seal(&corrupt_arena, 4096) ==
                ORES_RT_ARENA_OK,
            "corrupt-free-stack arena seal");
    OresRtArenaTurn corrupt_turn{};
    require(ores_rt_actor_arena_enter(&corrupt_arena, 4, 4, &corrupt_turn) ==
                ORES_RT_ARENA_OK,
            "corrupt-free-stack arena enter");

    const std::size_t original_free_count = corrupt_arena.free_count;
    const std::size_t top_slot =
        corrupt_arena.free_slots[original_free_count - 1];
    OresRtArenaHandle rejected{};

    corrupt_arena.free_slots[original_free_count - 1] =
        corrupt_arena.slot_count;
    require(ores_rt_actor_arena_alloc(
                &corrupt_arena, corrupt_turn, 8, &rejected) ==
                ORES_RT_ARENA_INVALID,
            "out-of-range free-stack entry must reject");
    require(corrupt_arena.free_count == original_free_count,
            "invalid free-stack entry must not consume capacity");

    corrupt_arena.free_slots[original_free_count - 1] = top_slot;
    corrupt_arena.slots[top_slot].live = 1;
    require(ores_rt_actor_arena_alloc(
                &corrupt_arena, corrupt_turn, 8, &rejected) ==
                ORES_RT_ARENA_INVALID,
            "live free-stack slot must reject");
    require(corrupt_arena.free_count == original_free_count,
            "live free-stack slot must not consume capacity");

    corrupt_arena.slots[top_slot].live = 0;
    corrupt_arena.slots[top_slot].generation =
        std::numeric_limits<std::uint64_t>::max();
    require(ores_rt_actor_arena_alloc(
                &corrupt_arena, corrupt_turn, 8, &rejected) ==
                ORES_RT_ARENA_GENERATION_EXHAUSTED,
            "generation-exhausted free-stack slot must reject");
    require(corrupt_arena.free_count == original_free_count,
            "generation exhaustion must not consume capacity");

    corrupt_arena.slots[top_slot].generation = 1;
    require(ores_rt_actor_arena_alloc(
                &corrupt_arena, corrupt_turn, 8, &rejected) ==
                ORES_RT_ARENA_OK,
            "restored free-stack slot must still allocate");
    require(corrupt_arena.free_count == original_free_count - 1,
            "successful allocation consumes exactly one free-stack entry");
    require(ores_rt_actor_arena_leave(&corrupt_arena, corrupt_turn) ==
                ORES_RT_ARENA_OK,
            "corrupt-free-stack arena leave");

    return 0;
}
