#include "oreslang/runtime/ActorArena.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } \
} while (false)

// Host-native scheduler/FFI-lifetime model. This does NOT run compiled AttoLang
// source or prove cancellation/FFI integration with the production scheduler.
int main() {
    constexpr int repetitions = 256;
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        const auto incarnation = static_cast<std::uint64_t>(iteration) + 1;
        OresActorArena* actor_a = ores_actor_arena_open(101, incarnation, 2, 64, 128);
        OresActorArena* actor_b = ores_actor_arena_open(202, incarnation, 2, 64, 128);
        CHECK(actor_a != nullptr && actor_b != nullptr);
        std::atomic<bool> active{false};
        std::atomic<bool> allow_leave{false};
        std::atomic<bool> left{false};
        OresArenaLoan loan{};
        std::thread carrier([&] {
            const auto turn = ores_actor_arena_enter(actor_a, 101, incarnation);
            CHECK(turn.epoch != 0);
            const auto handle = ores_actor_arena_alloc(actor_a, turn, sizeof(std::uint64_t));
            std::uint64_t value = 99;
            ores_actor_arena_write(actor_a, turn, handle, 0, &value, sizeof(value));
            loan = ores_actor_arena_borrow(actor_a, turn, handle);
            CHECK(loan.nonce != 0);
            active.store(true, std::memory_order_release);
            while (!allow_leave.load(std::memory_order_acquire)) std::this_thread::yield();
            // STOP fences all new slot reads/writes, but a previously admitted
            // turn must be permitted to unwind its scratch and carrier state.
            ores_actor_arena_leave(actor_a, turn);
            left.store(true, std::memory_order_release);
        });
        while (!active.load(std::memory_order_acquire)) std::this_thread::yield();
        ores_actor_arena_retire(actor_a);
        CHECK(ores_actor_arena_try_destroy(actor_a) == 0); // live turn + loan
        // An independent actor may enter its private arena during A's STOP.
        const auto other_turn = ores_actor_arena_enter(actor_b, 202, incarnation);
        const auto other_handle = ores_actor_arena_alloc(actor_b, other_turn, sizeof(std::uint64_t));
        const std::uint64_t expected = incarnation * 3;
        ores_actor_arena_write(actor_b, other_turn, other_handle, 0, &expected, sizeof(expected));
        std::uint64_t observed = 0;
        ores_actor_arena_read(actor_b, other_turn, other_handle, 0, &observed, sizeof(observed));
        CHECK(observed == expected);
        ores_actor_arena_leave(actor_b, other_turn);
        allow_leave.store(true, std::memory_order_release);
        carrier.join();
        CHECK(left.load(std::memory_order_acquire));
        CHECK(ores_actor_arena_try_destroy(actor_a) == 0); // external loan
        ores_actor_arena_release(actor_a, loan);
        CHECK(ores_actor_arena_try_destroy(actor_a) == 1);
        const auto other_final_turn = ores_actor_arena_enter(actor_b, 202, incarnation);
        ores_actor_arena_read(actor_b, other_final_turn, other_handle, 0, &observed, sizeof(observed));
        CHECK(observed == expected);
        ores_actor_arena_drop(actor_b, other_final_turn, other_handle);
        ores_actor_arena_leave(actor_b, other_final_turn);
        ores_actor_arena_retire(actor_b);
        CHECK(ores_actor_arena_try_destroy(actor_b) == 1);
    }
    std::printf("native-actor-stop-race: PASS (%d iterations)\n", repetitions);
    return 0;
}
