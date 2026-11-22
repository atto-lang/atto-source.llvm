// Trusted, single-actor M1 bridge for *compiled* .atto guest IR.
// The guest receives only a four-byte numeric token and field ordinals.
// This bridge does NOT authorize cross-actor transfer, arbitrary native guest
// code or an independent actor/tenant sandbox.
#include "oreslang/runtime/ActorRef32Heap.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
using namespace oreslang::runtime::vm;
constexpr ActorHeapOwner kOwner{1, 1, 1};
alignas(16) std::array<std::uint8_t, 8192> cage{};
ActorRef32Heap heap(kOwner, cage.data(), 1024, 4096, 2);
TrustedActorRef32 active{};

[[noreturn]] void reject() noexcept { std::abort(); }
void require_valid(std::uint32_t token, std::uint32_t field) noexcept {
    if (active.guest_token() == 0 || active.guest_token() != token || field > 2)
        reject();
}
}  // namespace

extern "C" std::uint32_t atto_m1_allocate() {
    if (active.guest_token()) reject();
    active = heap.allocate(kOwner, 24, 8, 101);
    if (!active.guest_token()) reject();
    return active.guest_token();
}

extern "C" void atto_m1_write(std::uint32_t token,
                               std::uint32_t field, std::int64_t value) {
    require_valid(token, field);
    if (!heap.write_i64(active, kOwner, 101, field * 8, value)) reject();
}

extern "C" std::int64_t atto_m1_read(std::uint32_t token, std::uint32_t field) {
    require_valid(token, field);
    std::int64_t result = 0;
    if (!heap.read_i64(active, kOwner, 101, field * 8, result)) reject();
    return result;
}

extern "C" int atto_m1_finish(std::uint32_t token, std::int64_t value) {
    require_valid(token, 0);
    if (value < 0 || value > 255) reject();
    if (!heap.release(active, kOwner, 101)) reject();
    active = TrustedActorRef32{};
    if (!heap.close_after_quiescence()) reject();
    std::cout << "ATTO_OBJECT_M1_RESULT=" << value << "\n"
              << "ATTO_OBJECT_M1_SCOPE=CLOSED_WORLD_SINGLE_ACTOR\n";
    return static_cast<int>(value);
}
