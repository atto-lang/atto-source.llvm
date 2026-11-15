#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

using oreslang::runtime::NativeTenantAdmission;

int main() {
    // External supervisor requests a stop while a physical actor turn runs.
    // A STOP must fence new turns but cannot recycle the backing region until
    // every admitted physical lease finishes. A different tenant stays live.
    constexpr unsigned kRounds = 256;
    NativeTenantAdmission vm(2, 2, 32);
    for (unsigned round = 1; round <= kRounds; ++round) {
        const auto target = vm.open(), peer = vm.open();
        assert(target && peer && target->slot != peer->slot);
        std::atomic<bool> active{false};
        std::atomic<bool> allow_finish{false};
        std::atomic<bool> turn_finished{false};
        std::thread actor([&] {
            auto turn = vm.enter(*target, 0);
            assert(turn);
            const std::uint64_t original = (static_cast<std::uint64_t>(round) << 32) | 0x1234;
            assert(turn->write(0, &original, sizeof(original)));
            active.store(true, std::memory_order_release);
            while (!allow_finish.load(std::memory_order_acquire))
                std::this_thread::yield();
            std::uint64_t observed = 0;
            assert(turn->read(0, &observed, sizeof(observed)) && observed == original);
            const std::uint64_t revised = original + 1;
            // A pre-existing physical turn may complete during quiescence.
            assert(turn->write(0, &revised, sizeof(revised)));
            assert(turn->read(0, &observed, sizeof(observed)) && observed == revised);
            turn.reset();
            turn_finished.store(true, std::memory_order_release);
        });
        while (!active.load(std::memory_order_acquire))
            std::this_thread::yield();
        assert(vm.request_stop(*target));
        assert(!vm.request_stop(*target));
        assert(!vm.enter(*target, 0));
        assert(!vm.enter(*target, 1));
        assert(!vm.finish_stop(*target));
        assert(!vm.open());
        // Tenant-scoped STOP must not incorrectly revoke neighboring tenants.
        auto peer_turn = vm.enter(*peer, 0); assert(peer_turn);
        std::uint64_t peer_value = 0xfeed0000ULL + round;
        std::uint64_t observed = 0;
        assert(peer_turn->write(0, &peer_value, sizeof(peer_value)));
        assert(peer_turn->read(0, &observed, sizeof(observed)) && observed == peer_value);
        peer_turn.reset();
        allow_finish.store(true, std::memory_order_release);
        actor.join();
        assert(turn_finished.load(std::memory_order_acquire));
        assert(vm.finish_stop(*target));
        // An old generation may not be reentered, re-stopped, or finalized.
        assert(!vm.enter(*target, 0) && !vm.finish_stop(*target));
        const auto reincarnated = vm.open(); assert(reincarnated);
        assert(reincarnated->slot == target->slot);
        assert(reincarnated->generation > target->generation);
        auto reincarnated_turn = vm.enter(*reincarnated, 0);
        assert(reincarnated_turn);
        observed = UINT64_MAX;
        assert(reincarnated_turn->read(0, &observed, sizeof(observed)) && observed == 0);
        reincarnated_turn.reset();
        assert(vm.request_stop(*reincarnated) && vm.finish_stop(*reincarnated));
        assert(vm.request_stop(*peer) && vm.finish_stop(*peer));
    }
}
