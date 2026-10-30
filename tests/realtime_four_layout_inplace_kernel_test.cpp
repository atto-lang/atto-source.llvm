#include "oreslang/runtime/DopHeterogeneousLayout.hpp"
#include "oreslang/runtime/RealtimeActorArena.h"
#include "oreslang/runtime/RealtimeColumnKernel.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <vector>

using oreslang::dop::FieldSpec;
using oreslang::dop::HeterogeneousLayout;
using oreslang::dop::HeterogeneousPlan;
using oreslang::runtime::realtime::add_i64_cells;
using oreslang::runtime::realtime::ColumnAddStatus;
using oreslang::runtime::realtime::read_u64_le;
using oreslang::runtime::realtime::write_u64_le;

static std::atomic<unsigned> hot_new_calls{0};
static std::atomic<bool> hot{false};
void* operator new(std::size_t bytes) {
    if (hot.load(std::memory_order_relaxed))
        hot_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(bytes)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t bytes) {
    if (hot.load(std::memory_order_relaxed))
        hot_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(bytes)) return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void require(bool value) { if (!value) std::abort(); }
static void put(OresRealtimeActorArena* arena, OresRtArenaTurn turn,
                OresRtArenaHandle handle, std::uint64_t at,
                std::uint64_t value) {
    std::array<unsigned char,8> bytes{};
    write_u64_le(bytes.data(),value);
    require(ores_rt_actor_arena_write(arena,turn,handle,
                static_cast<std::size_t>(at),bytes.data(),bytes.size()) ==
            ORES_RT_ARENA_OK);
}
static std::uint64_t get(OresRealtimeActorArena* arena, OresRtArenaTurn turn,
                         OresRtArenaHandle handle, std::uint64_t at) {
    std::array<unsigned char,8> bytes{};
    require(ores_rt_actor_arena_read(arena,turn,handle,
                static_cast<std::size_t>(at),bytes.data(),bytes.size()) ==
            ORES_RT_ARENA_OK);
    return read_u64_le(bytes.data());
}

static void run(HeterogeneousLayout layout) {
    constexpr std::uint64_t rows = 17; // AoSoA exercises a masked tail tile.
    const std::vector<FieldSpec> fields{{8,8},{8,8},{1,1}};
    const std::vector<std::vector<std::uint32_t>> groups{{0,2},{1}};
    const HeterogeneousPlan plan(layout,rows,fields,
        layout == HeterogeneousLayout::AoSoA ? 8 : 1,
        layout == HeterogeneousLayout::SoAoS ? groups :
        std::vector<std::vector<std::uint32_t>>{});
    require(plan.physical_bytes()>0 && plan.physical_bytes()<4096);

    alignas(std::max_align_t) std::array<unsigned char,16384> backing{};
    OresRealtimeActorArena arena{};
    require(ores_rt_actor_arena_init(&arena,backing.data(),backing.size(),
                101,202,303,1,4096,256)==ORES_RT_ARENA_OK);
    require(ores_rt_actor_arena_seal(&arena,4096)==ORES_RT_ARENA_OK);
    OresRtArenaTurn turn{};
    require(ores_rt_actor_arena_enter(&arena,202,303,&turn)==ORES_RT_ARENA_OK);
    OresRtArenaHandle buffer{};
    require(ores_rt_actor_arena_alloc(&arena,turn,
                static_cast<std::size_t>(plan.physical_bytes()),&buffer) ==
            ORES_RT_ARENA_OK);

    for(std::uint64_t row=0;row<rows;++row) {
        std::uint64_t pos=0,velocity=0;
        require(plan.try_absolute_address(row,0,&pos));
        require(plan.try_absolute_address(row,1,&velocity));
        put(&arena,turn,buffer,pos,row+10);
        put(&arena,turn,buffer,velocity,2);
    }

    // No object allocation, intermediate copy, thrown exception or pointer
    // escape is permitted in this bounded actor-owned update loop.
    const auto before=hot_new_calls.load(std::memory_order_relaxed);
    hot.store(true,std::memory_order_relaxed);
    for(std::uint64_t row=0;row<rows;++row) {
        std::uint64_t pos=0,velocity=0;
        require(plan.try_absolute_address(row,0,&pos));
        require(plan.try_absolute_address(row,1,&velocity));
        require(add_i64_cells(&arena,turn,buffer,pos,buffer,velocity)==
                ColumnAddStatus::Ok);
    }
    hot.store(false,std::memory_order_relaxed);
    require(hot_new_calls.load(std::memory_order_relaxed)==before);

    for(std::uint64_t row=0;row<rows;++row) {
        std::uint64_t pos=0;
        require(plan.try_absolute_address(row,0,&pos));
        require(get(&arena,turn,buffer,pos)==row+12);
    }
    std::uint64_t pos=0,velocity=0;
    require(plan.try_absolute_address(0,0,&pos));
    require(plan.try_absolute_address(0,1,&velocity));

    // Transactional overflow: the destination must remain unchanged.
    put(&arena,turn,buffer,pos,static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max()));
    put(&arena,turn,buffer,velocity,1);
    require(add_i64_cells(&arena,turn,buffer,pos,buffer,velocity)==
            ColumnAddStatus::Overflow);
    require(get(&arena,turn,buffer,pos)==
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    put(&arena,turn,buffer,pos,std::uint64_t{1}<<63);
    put(&arena,turn,buffer,velocity,~std::uint64_t{0});
    require(add_i64_cells(&arena,turn,buffer,pos,buffer,velocity)==
            ColumnAddStatus::Overflow);
    require(get(&arena,turn,buffer,pos)==(std::uint64_t{1}<<63));

    require(add_i64_cells(&arena,turn,buffer,pos+1,buffer,velocity)==
            ColumnAddStatus::OutOfBounds);
    require(add_i64_cells(&arena,turn,buffer,plan.physical_bytes(),
                buffer,velocity)==ColumnAddStatus::OutOfBounds);
    require(add_i64_cells(&arena,turn,buffer,pos,buffer,
                std::numeric_limits<std::uint64_t>::max())==
            ColumnAddStatus::OutOfBounds);

    // TLS carrier identity makes a stolen turn unusable on another thread.
    ColumnAddStatus foreign=ColumnAddStatus::Ok;
    std::thread other([&]{foreign=add_i64_cells(&arena,turn,buffer,pos,buffer,velocity);});
    other.join();
    require(foreign==ColumnAddStatus::Denied);

    require(ores_rt_actor_arena_leave(&arena,turn)==ORES_RT_ARENA_OK);
    require(add_i64_cells(&arena,turn,buffer,pos,buffer,velocity)==
            ColumnAddStatus::Denied);
    OresRtArenaTurn next{};
    require(ores_rt_actor_arena_enter(&arena,202,303,&next)==ORES_RT_ARENA_OK);
    require(ores_rt_actor_arena_drop(&arena,next,buffer)==ORES_RT_ARENA_OK);
    require(add_i64_cells(&arena,next,buffer,pos,buffer,velocity)==
            ColumnAddStatus::Denied);
    require(ores_rt_actor_arena_leave(&arena,next)==ORES_RT_ARENA_OK);
    require(ores_rt_actor_arena_retire(&arena)==ORES_RT_ARENA_OK);
}
int main() {
    for (auto layout : {HeterogeneousLayout::AoS,HeterogeneousLayout::SoA,
                        HeterogeneousLayout::AoSoA,HeterogeneousLayout::SoAoS})
        run(layout);
}
