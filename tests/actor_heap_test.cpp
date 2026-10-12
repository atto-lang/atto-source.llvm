#include "oreslang/runtime/ActorHeap.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorHeap;
static void require(bool p, const char* why) {
    if (!p) throw std::runtime_error(why);
}

int main() {
    try {
        // Multiple startup threads must not race on the owner identity.
        ActorHeap<2, 16> contested;
        std::atomic<int> ready{0}, admitted{0};
        std::atomic<bool> begin{false};
        std::vector<std::thread> binders;
        for (int i = 0; i < 32; ++i) {
            binders.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!begin.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if (contested.bind_carrier())
                    admitted.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (ready.load(std::memory_order_acquire) != 32)
            std::this_thread::yield();
        begin.store(true, std::memory_order_release);
        for (auto& binder : binders) binder.join();
        require(admitted.load() == 1,
                "concurrent actor heap admission selected multiple owners");

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
        // A retired actor's native heap must not grant fresh storage or
        // preserve any stale generation, including if live slots remained.
        ActorHeap<2, 16> retirement;
        ActorHeap<2, 16> unaffected;
        require(retirement.bind_carrier() && unaffected.bind_carrier(),
                "retirement-test carriers not admitted");
        auto alive = retirement.allocate(8);
        auto separate = unaffected.allocate(8);
        require(alive && separate, "retirement fixture allocation failed");
        std::uint64_t sentinel = 0x12345678;
        require(retirement.write(*alive, 0, &sentinel, sizeof sentinel),
                "retirement fixture write failed");
        std::atomic<bool> foreign_retire_denied{false};
        std::thread foreign_retire([&] {
            foreign_retire_denied.store(!retirement.retire(), std::memory_order_release);
        });
        foreign_retire.join();
        require(foreign_retire_denied.load(std::memory_order_acquire),
                "foreign carrier retired an actor heap");
        require(retirement.retire() && retirement.retired() && retirement.retire(),
                "actor-owned retirement failed or was not idempotent");
        require(!retirement.allocate(8), "retired heap admitted an allocation");
        require(!retirement.read(*alive, 0, &observed, sizeof observed) &&
                !retirement.write(*alive, 0, &sentinel, sizeof sentinel) &&
                !retirement.release(*alive),
                "retired actor handle remained usable");
        require(retirement.live_blocks() == 0 && retirement.live_bytes() == 0,
                "retired actor heap retained live accounting");
        require(unaffected.read(*separate, 0, &observed, sizeof observed) &&
                unaffected.release(*separate),
                "retiring one actor changed another actor heap");

        // Deterministic state-machine stress: compare thousands of allocator
        // transitions with an independently maintained live-slot count.
        // ASan/UBSan should also catch stale byte accesses or invalid writes.
        ActorHeap<8, 16> modeled;
        require(modeled.bind_carrier(), "model heap admission failed");
        std::array<std::optional<ActorHeap<8, 16>::Handle>, 8> handles{};
        std::array<std::uint64_t, 8> values{};
        std::uint32_t seed = 0xc001d00dU;
        std::size_t count = 0;
        for (std::size_t step = 0; step < 12000; ++step) {
            seed = seed * 1664525U + 1013904223U;
            const std::size_t id = static_cast<std::size_t>(seed % 8U);
            if (handles[id]) {
                std::uint64_t got = 0;
                require(modeled.read(*handles[id], 0, &got, sizeof got)
                            && got == values[id], "model read mismatch");
                if ((seed & 3U) == 0U) {
                    auto staleHandle = *handles[id];
                    require(modeled.release(staleHandle), "model release rejected");
                    handles[id].reset();
                    --count;
                    require(!modeled.read(staleHandle, 0, &got, sizeof got)
                                && !modeled.release(staleHandle),
                            "model stale handle reused");
                } else {
                    values[id] = static_cast<std::uint64_t>(seed) << 8;
                    require(modeled.write(*handles[id], 0, &values[id],
                                          sizeof values[id]), "model write rejected");
                }
            } else {
                auto created = modeled.allocate(sizeof(std::uint64_t));
                if (created) {
                    std::uint64_t zero = 1234;
                    require(modeled.read(*created, 0, &zero, sizeof zero)
                                && zero == 0, "recycled slot not zeroized");
                    handles[id] = *created;
                    values[id] = static_cast<std::uint64_t>(seed);
                    require(modeled.write(*created, 0, &values[id],
                                          sizeof values[id]), "model initialize failed");
                    ++count;
                } else {
                    require(count == handles.size(), "premature model exhaustion");
                }
            }
            require(modeled.live_blocks() == count
                        && modeled.live_bytes() == count * sizeof(std::uint64_t),
                    "model accounting invariant violated");
        }
        require(modeled.retire(), "modeled heap retirement failed");
        require(modeled.retire() && !modeled.allocate(sizeof(std::uint64_t)),
                "retirement did not fence later model admission");
        for (auto& handle : handles) {
            if (handle) {
                std::uint64_t got = 0;
                require(!modeled.read(*handle, 0, &got, sizeof got),
                        "live pre-retirement handle escaped shutdown");
            }
        }
        require(modeled.live_blocks() == 0 && modeled.live_bytes() == 0,
                "retired modeled heap retains memory usage");

        std::cout << "PASS actor heap ownership, reclamation, ABA, retirement fence, model stress, bounds and concurrency\n";
    } catch (const std::exception& failure) {
        std::cerr << "FAIL actor heap: " << failure.what() << "\n";
        return 1;
    }
}
