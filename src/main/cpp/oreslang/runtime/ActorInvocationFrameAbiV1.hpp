#pragma once
#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/ActorInvocationFrames.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace oreslang::runtime::abi {

// Versioned, pointer-free native frame ABI shape for future LLVM IR lowering.
// These structures are POD metadata, NOT capabilities or proof of a valid
// scheduler lease. All guest emission remains denied until source verifier,
// runtime checks, and actual nested suspends guest tests are integrated.
inline constexpr std::uint32_t kActorInvocationFramesAbiVersion=1;

struct ActorInvocationTokenV1 {
    std::uint64_t actor=0;
    std::uint64_t generation=0;
    std::uint64_t sequence=0;
    std::uint64_t slot_generation=0;
    std::uint32_t slot=0;
    std::uint32_t reserved=0;
};

struct ActorCallFrameV1 {
    ActorInvocationTokenV1 owner{};
    std::uint64_t nonce=0;
    std::uint32_t depth=0;
    std::uint32_t reserved=0;
};

static_assert(std::is_standard_layout_v<ActorInvocationTokenV1>);
static_assert(std::is_trivially_copyable_v<ActorInvocationTokenV1>);
static_assert(sizeof(ActorInvocationTokenV1)==40);
static_assert(alignof(ActorInvocationTokenV1)==8);
static_assert(sizeof(ActorCallFrameV1)==56);
static_assert(alignof(ActorCallFrameV1)==8);

template<std::size_t Capacity>
ActorInvocationTokenV1 encode(
    typename ActorExecutionLedger<Capacity>::Token token) noexcept {
    return {token.actor,token.generation,token.sequence,
            token.slot_generation,token.slot,0};
}

template<std::size_t Capacity>
bool decode(ActorInvocationTokenV1 wire,
    typename ActorExecutionLedger<Capacity>::Token& out) noexcept {
    if (wire.reserved!=0 || wire.actor==0 || wire.generation==0 ||
        wire.sequence==0 || wire.slot_generation==0 || wire.slot>=Capacity)
        return false;
    out={wire.actor,wire.generation,wire.sequence,
         wire.slot_generation,wire.slot};
    return true;
}

template<std::size_t Capacity, std::size_t Depth, std::size_t StackBytes,
         std::size_t ActorBytes>
ActorCallFrameV1 encode(
    typename ActorInvocationFrames<Capacity,Depth,StackBytes,ActorBytes>::FrameRef ref) noexcept {
    return {encode<Capacity>(ref.owner),ref.nonce,ref.depth,0};
}

// The physical actor lease and live incarnation must be authenticated by
// ActorInvocationFrameBridge after decode. "Well formed" is never "authorized".
template<std::size_t Capacity, std::size_t Depth, std::size_t StackBytes,
         std::size_t ActorBytes>
bool decode(ActorCallFrameV1 wire,
    typename ActorInvocationFrames<Capacity,Depth,StackBytes,ActorBytes>::FrameRef& out) noexcept {
    if (wire.reserved!=0 || wire.nonce==0 ||
        wire.depth==0 || wire.depth>Depth) return false;
    typename ActorExecutionLedger<Capacity>::Token owner{};
    if (!decode<Capacity>(wire.owner,owner)) return false;
    out={owner,wire.nonce,wire.depth};
    return true;
}

} // namespace oreslang::runtime::abi
