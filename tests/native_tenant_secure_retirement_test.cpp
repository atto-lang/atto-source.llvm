#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>

using oreslang::runtime::NativeTenantAdmission;

// Observe real arena deletion without exposing an arena pointer from the VM.
// This fixed-size array allocation is unique to the fixture; unrelated C++
// new[] allocations are delegated to the same malloc/free implementation.
namespace {
constexpr std::size_t kArenaBytes = 313;
std::atomic<void*> observed_arena{nullptr};
std::atomic<unsigned> deleted_arenas{0};
std::atomic<unsigned> dirty_arenas{0};
std::atomic<unsigned> constructed_arenas{0};
}

void* operator new[](std::size_t bytes) {
    void* memory = std::malloc(bytes == 0 ? 1 : bytes);
    if (!memory) throw std::bad_alloc();
    if (bytes == kArenaBytes) {
        assert(observed_arena.exchange(memory) == nullptr);
        constructed_arenas.fetch_add(1);
    }
    return memory;
}

void operator delete[](void* memory) noexcept {
    if (!memory) return;
    if (observed_arena.load() == memory) {
        auto* ptr = static_cast<volatile std::uint8_t*>(memory);
        bool clean = true;
        for (std::size_t i = 0; i < kArenaBytes; ++i)
            if (ptr[i] != 0) clean = false;
        if (!clean) dirty_arenas.fetch_add(1);
        deleted_arenas.fetch_add(1);
        observed_arena.store(nullptr);
    }
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    operator delete[](memory);
}

static void fill(NativeTenantAdmission::Turn& turn) {
    std::array<std::uint8_t, kArenaBytes> secret{};
    for (std::size_t i = 0; i < secret.size(); ++i)
        secret[i] = static_cast<std::uint8_t>(i % 251 + 1);
    assert(turn.write(0, secret.data(), secret.size()));
}

static void check_one_more(unsigned target) {
    assert(constructed_arenas.load() == target);
    assert(deleted_arenas.load() == target);
    assert(dirty_arenas.load() == 0);
    assert(observed_arena.load() == nullptr);
}

int main() {
    // Normal STOP has always scrubbed; the final deallocation must as well.
    {
        NativeTenantAdmission vm(1, 1, kArenaBytes);
        const auto h = vm.open(); assert(h);
        auto turn = vm.enter(*h, 0); assert(turn);
        fill(*turn);
        turn.reset();
        assert(vm.request_stop(*h) && vm.finish_stop(*h));
    }
    check_one_more(1);

    // Abnormal destruction WITHOUT request_stop/finish_stop must scrub.
    {
        NativeTenantAdmission vm(1, 1, kArenaBytes);
        const auto h = vm.open(); assert(h);
        auto turn = vm.enter(*h, 0); assert(turn);
        fill(*turn);
        turn.reset();
    }
    check_one_more(2);

    // A surviving Turn pins state, but cannot touch state after host teardown;
    // final retirement waits for the physical lease to release.
    std::optional<NativeTenantAdmission::Turn> late;
    {
        NativeTenantAdmission vm(1, 1, kArenaBytes);
        const auto h = vm.open(); assert(h);
        late = vm.enter(*h, 0); assert(late);
        fill(*late);
    }
    assert(deleted_arenas.load() == 2);
    std::uint8_t out = 0;
    assert(!late->read(0, &out, 1));
    late.reset();
    check_one_more(3);

    // Quiescence with a live pin: finish_stop must fail; owner teardown and
    // last pin release still scrub the byte region exactly once.
    {
        std::optional<NativeTenantAdmission::Turn> blocked;
        {
            NativeTenantAdmission vm(1, 1, kArenaBytes);
            const auto h = vm.open(); assert(h);
            blocked = vm.enter(*h, 0); assert(blocked);
            fill(*blocked);
            assert(vm.request_stop(*h));
            assert(!vm.finish_stop(*h));
        }
        assert(deleted_arenas.load() == 3);
        assert(!blocked->write(0, &out, 1));
        blocked.reset();
    }
    check_one_more(4);
}
