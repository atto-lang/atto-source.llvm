#include "oreslang/runtime/NativeScalarState.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::CriticalGate;
using oreslang::runtime::NativeScalarState;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template <class F> static void expect_rejected(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    check(rejected, "unsafe native actor loan was admitted");
}

int main() {
    constexpr std::uint64_t actor = 501;
    constexpr std::uint64_t incarnation = 7;
    auto* arena = ores_actor_arena_open(actor, incarnation, 2, 8, 64);
    const auto init = ores_actor_arena_enter(arena, actor, incarnation);
    const auto count = ores_actor_arena_alloc(arena, init, 8);
    const auto enabled = ores_actor_arena_alloc(arena, init, 8);
    const std::int64_t starting = 1;
    ores_actor_arena_write(arena, init, count, 0, &starting, sizeof(starting));
    ores_actor_arena_leave(arena, init);

    CriticalGate gate;
    NativeScalarState state(gate, arena, std::vector<OresArenaHandle>{count, enabled});
    const auto first = gate.request();
    check(first.admission == CriticalGate::Admission::acquired,
          "first logical continuation must enter immediately");
    const auto second = gate.request();
    const auto cancelled = gate.request();
    check(second.admission == CriticalGate::Admission::queued,
          "contended logical continuation must queue instead of park");
    check(!gate.release(cancelled.token), "queued cancellation granted a critical ticket");

    const auto turn = ores_actor_arena_enter(arena, actor, incarnation);
    {
        auto outer = state.borrow(first.token, turn);
        check(outer.read(0) == 1, "persistent native initial value lost");
        outer.write(0, 3);
        expect_rejected([&] { state.assert_can_suspend(); });
        {
            auto inner = state.borrow(first.token, turn);
            expect_rejected([&] { (void) outer.read(0); });
            inner.write(0, 7);
        }
        check(outer.read(0) == 7, "nested borrow failed to restore outer access");
        expect_rejected([&] { (void) outer.read(2); });
        outer.close();
        expect_rejected([&] { (void) outer.read(0); });
    }
    state.assert_can_suspend();
    // A prematurely ended outer loan must not be resurrected after its
    // nested view unwinds, even when cleanup is out of lexical order.
    {
        auto outer = state.borrow(first.token, turn);
        auto nested = state.borrow(first.token, turn);
        outer.close();
        expect_rejected([&] { state.assert_can_suspend(); });
        nested.close();
        state.assert_can_suspend();
        expect_rejected([&] { (void) outer.read(0); });
    }
    ores_actor_arena_leave(arena, turn);

    check(gate.release(first.token) == second.token,
          "FIFO logical handoff must be independent of carrier thread");
    expect_rejected([&] {
        const auto transferred = ores_actor_arena_enter(arena, actor, incarnation);
        try {
            auto stale = state.borrow(first.token, transferred);
            (void) stale;
        } catch (...) {
            ores_actor_arena_leave(arena, transferred);
            throw;
        }
        ores_actor_arena_leave(arena, transferred);
    });

    std::atomic<bool> migrated{false};
    std::thread newCarrier([&] {
        const auto resumed = ores_actor_arena_enter(arena, actor, incarnation);
        {
            auto view = state.borrow(second.token, resumed);
            check(view.read(0) == 7, "carrier migration discarded persistent state");
            view.write(0, 11);
        }
        state.assert_can_suspend();
        ores_actor_arena_leave(arena, resumed);
        migrated.store(true, std::memory_order_release);
    });
    newCarrier.join();
    check(migrated.load(std::memory_order_acquire),
          "logical critical ticket did not resume on different carrier");
    check(!gate.release(second.token), "unexpected remaining queued critical owner");

    // Native loans pin physical reclamation, but retirement revokes access.
    const auto finalTurn = ores_actor_arena_enter(arena, actor, incarnation);
    const auto loan = ores_actor_arena_borrow(arena, finalTurn, count);
    ores_actor_arena_leave(arena, finalTurn);
    state.retire();
    check(state.retired(), "state retirement not recorded");
    expect_rejected([&] {
        auto view = state.borrow(second.token, finalTurn);
        (void) view;
    });
    ores_actor_arena_retire(arena);
    check(ores_actor_arena_try_destroy(arena) == 0,
          "retirement freed arena despite outstanding native loan");
    ores_actor_arena_release(arena, loan);
    check(ores_actor_arena_try_destroy(arena) == 1,
          "native arena was not reclaimed after last loan released");

    return 0;
}
