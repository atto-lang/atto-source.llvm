#include "oreslang/runtime/RealtimeColumnKernel.hpp"

#include <cstdint>

// Isolated host-codegen object used to audit the trusted hot-path call closure.
// No planner constructor, guest code, FFI or scheduler is linked by this probe.
extern "C" int ores_rt_column_kernel_object_probe(
    OresRealtimeActorArena* arena,
    OresRtArenaTurn turn,
    OresRtArenaHandle destination,
    std::uint64_t destination_offset,
    OresRtArenaHandle source,
    std::uint64_t source_offset) noexcept {
    return static_cast<int>(oreslang::runtime::realtime::add_i64_cells(
        arena,turn,destination,destination_offset,source,source_offset));
}
