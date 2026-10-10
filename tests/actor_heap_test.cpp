#include "oreslang/runtime/ActorHeap.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorHeap;
static void require(bool p, const char* why) {
    if (!p) throw std::runtime_error(why);
}

int main() {
    try {
        ActorHeap<2, 16> a, b;
        require(!a.allocate(8), "allocation succeeded before carrier binding");
        require(a.bind_carrier() && b.bind_carrier(), "carrier admission failed");
        auto h1 = a.allocate(sizeof(std::uint64_t));
        auto h2 = a.allocate(16);
        require(h1 && h2 && !a.allocate(1), "heap quota not enforced");
        require(!a.allocate(17), "oversized allocation accepted");
        std::uint64_t number = 0xabcdef;
        require(a.write(*h1, 0, &number, sizeof number), "write rejected");
        std::uint64_t observed = 0;
        require(a.read(*h1, 0, &observed, sizeof observed) && observed == number,
                "native heap did not preserve state");
        require(!a.write(*h1, 4, &number, sizeof number), "out of bounds write");
        require(!b.read(*h1, 0, &observed, sizeof observed),
                "other actor accepted a foreign heap handle");
        require(!a.read(ActorHeap<2, 16>::Handle{0, 0, 1}, 0,
                        &observed, sizeof observed), "invalid handle accepted");
        require(a.release(*h1), "reclamation failed");
        require(!a.release(*h1) && !a.read(*h1, 0, &observed, sizeof observed),
                "stale generation accepted");
        auto fresh = a.allocate(sizeof(std::uint64_t));
        require(fresh && fresh->slot == h1->slot &&
                fresh->generation != h1->generation,
                "slot ABA protection failed");
        require(a.read(*fresh, 0, &observed, sizeof observed) && observed == 0,
                "released slot leaked old bytes");
        require(a.live_blocks() == 2 && a.live_bytes() == 24,
                "native heap accounting inconsistent");
        bool rejected = false;
        std::thread rogue([&] {
            std::uint64_t temp = 1;
            rejected = !a.read(*fresh, 0, &temp, sizeof temp) &&
                       !a.write(*fresh, 0, &temp, sizeof temp) &&
                       !a.release(*fresh) &&
                       !a.bind_carrier();
        });
        rogue.join();
        require(rejected, "foreign thread accessed carrier-private heap");
        require(a.release(*fresh) && a.release(*h2), "release failed");
        require(a.live_blocks() == 0 && a.live_bytes() == 0,
                "heap is not empty after reclamation");
        std::cout << "PASS actor heap ownership, reclamation, ABA, bounds and concurrency\n";
    } catch (const std::exception& failure) {
        std::cerr << "FAIL actor heap: " << failure.what() << "\n";
        return 1;
    }
}
