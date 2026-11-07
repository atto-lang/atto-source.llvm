#include "oreslang/runtime/ActorFrameArena.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorFrameArena;

static void require(bool yes, const char* message) {
    if (!yes) throw std::runtime_error(message);
}

int main() {
    try {
        ActorFrameArena<3, 32> first, second;
        require(first.enter(9), "first actor admission failed");
        auto bottom = first.push(8);
        auto top = first.push(8);
        require(bottom && top, "actor frame allocation failed");
        std::uint64_t sentinel = 0xcafe1234;
        std::uint64_t observed = 0;
        require(first.write(*bottom, 0, &sentinel, sizeof sentinel),
                "failed to store an owned local");
        require(!first.write(*bottom, 1, &sentinel, sizeof sentinel),
                "out of bounds frame store accepted");
        require(!second.read(*bottom, 0, &observed, sizeof observed),
                "foreign actor read first actor's frame");
        require(!second.enter(5), "reentrant actor admission on same carrier");
        ActorFrameArena<1, 8> another_capacity;
        require(!another_capacity.enter(7),
                "nested actor admission crossed template capacity domains");
        require(!first.retire_quiesced(), "retired while a physical turn was active");
        require(first.suspend(), "await failed to preserve logical stack");
        require(!first.read(*bottom, 0, &observed, sizeof observed),
                "supervisor read suspended actor locals");
        require(!first.enter(10), "unrelated execution stole suspended frames");

        std::atomic<bool> migration_ok{false};
        std::thread migrated([&] {
            const bool ok = first.enter(9) &&
                first.read(*bottom, 0, &observed, sizeof observed) &&
                observed == sentinel &&
                first.active_frames() == 2 && first.active_bytes() == 16 &&
                first.pop(*top) && first.pop(*bottom) && first.finish();
            migration_ok.store(ok, std::memory_order_release);
        });
        migrated.join();
        require(migration_ok.load(std::memory_order_acquire),
                "cross-carrier continuation did not preserve frame contents");
        require(!first.read(*bottom, 0, &observed, sizeof observed),
                "stale frame usable outside an actor turn");
        require(first.enter(11), "next actor logical execution was rejected");
        auto recycled = first.push(8);
        require(recycled && first.read(*recycled, 0, &observed, sizeof observed)
                    && observed == 0, "recycled frame bytes were not scrubbed");
        require(!first.read(*bottom, 0, &observed, sizeof observed),
                "old frame capability survived execution generation");
        require(first.pop(*recycled) && first.finish(), "new turn failed cleanup");
        require(first.retire_quiesced() && first.retire_quiesced(),
                "quiescent actor retirement not idempotent");
        require(first.retired() && !first.enter(9), "retired actor resumed");

        // Exhaustion is an actor-local terminal failure, not a native recursion
        // or host SIGSEGV; another incarnation remains healthy.
        ActorFrameArena<2, 8> failing, healthy;
        require(failing.enter(3), "failed to admit stack budget test");
        auto x = failing.push(4);
        auto y = failing.push(4);
        require(x && y && !failing.push(1) && failing.poisoned(),
                "frame-depth/byte quota was bypassed");
        require(!failing.suspend() && failing.cancel_active(),
                "failed to reclaim poisoned stack");
        require(!failing.enter(4) && failing.retire_quiesced(),
                "poisoned actor was re-admitted");
        require(healthy.enter(3), "healthy sibling actor failed after exhaustion");
        auto good = healthy.push(8);
        require(good && healthy.pop(*good) && healthy.finish(),
                "healthy sibling frame operations failed");
        require(healthy.retire_quiesced(), "healthy sibling failed retirement");

        // Byte quota must fail independently from the number-of-frames cap.
        ActorFrameArena<3, 8> byte_budget;
        require(byte_budget.enter(42), "byte-budget admission failed");
        auto almost_full = byte_budget.push(6);
        require(almost_full && !byte_budget.push(3) && byte_budget.poisoned(),
                "actor byte budget was bypassed below frame-count cap");
        require(byte_budget.cancel_active() && byte_budget.retire_quiesced(),
                "byte-budget poisoning left actor storage alive");

        ActorFrameArena<2, 16> invalid_order;
        require(invalid_order.enter(7), "non-LIFO setup failed");
        auto one = invalid_order.push(8);
        auto two = invalid_order.push(8);
        require(one && two && !invalid_order.pop(*one) && invalid_order.poisoned(),
                "out-of-order unwind did not poison actor stack");
        require(!invalid_order.write(*two, 0, &sentinel, sizeof sentinel) &&
                invalid_order.cancel_active() && invalid_order.retire_quiesced(),
                "poisoned arena leaked write authority");

        ActorFrameArena<1, 8> reincarnated;
        require(reincarnated.enter(9) && !reincarnated.read(*bottom, 0,
                    &observed, sizeof observed), "restarted actor accepted stale stack token");
        auto new_frame = reincarnated.push(8);
        require(new_frame && reincarnated.pop(*new_frame) && reincarnated.finish() &&
                reincarnated.retire_quiesced(), "new incarnation failed");

        for (int i = 0; i < 4096; ++i) {
            ActorFrameArena<1, 8> round;
            require(round.enter(1), "model-turn admission failed");
            auto frame = round.push(8);
            require(frame && round.write(*frame, 0, &sentinel, sizeof sentinel) &&
                    round.read(*frame, 0, &observed, sizeof observed) &&
                    observed == sentinel && round.pop(*frame) && round.finish() &&
                    round.retire_quiesced(), "model allocation/retirement mismatch");
        }
        std::cout << "PASS actor-owned frame bytes: isolation, migration, "
                  << "suspension, non-LIFO poison, sibling survival, generation, "
                  << "4096 retirement cycles\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL actor frame arena: " << error.what() << '\n';
        return 1;
    }
}
