// Host-only regression: one compiled callback definition can serve several
// actor heaps and supervisor control logic without making object state global.
// This is NOT proof of .atto guest lowering, OS page mappings, or isolation
// against privileged native code that already holds a Heap&.
#include "oreslang/runtime/ActorOwnedHeap.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
using Heap = ActorOwnedHeap<128, 4>;
using Handle = Heap::Handle;
using Code = std::int64_t (*)(std::int64_t, std::int64_t);

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// The same implementation is used for private actor state and for the
// supervisor's private bookkeeping. Neither contains hidden mutable globals.
static std::int64_t shared_instruction(std::int64_t value,
                                       std::int64_t increment) noexcept {
    return value + increment;
}

static bool actor_turn(Heap& heap, const Handle& state,
                       Code implementation, std::int64_t delta) noexcept {
    const auto previous = heap.read<std::int64_t>(state);
    return previous.has_value()
        && heap.write(state, implementation(*previous, delta));
}

static void test_shared_code_with_independent_actor_and_supervisor_state() {
    const ActorHeapIdentity alice_id{902, 21, 1};
    const ActorHeapIdentity bob_id{902, 22, 1};
    Heap alice(alice_id);
    Heap bob(bob_id);
    const auto a = alice.allocate(sizeof(std::int64_t), alignof(std::int64_t));
    const auto b = bob.allocate(sizeof(std::int64_t), alignof(std::int64_t));
    require(a.has_value() && b.has_value(), "independent actor allocation failed");
    require(alice.write(*a, std::int64_t{10}), "alice initialization failed");
    require(bob.write(*b, std::int64_t{100}), "bob initialization failed");

    // A single function pointer is deliberately passed to each worker and
    // the supervisor. Actor instances do not own copies of this definition.
    const Code shared = &shared_instruction;
    constexpr int turns = 4096;
    std::atomic<bool> workers_ok{true};
    std::thread worker_a([&] {
        for (int i = 0; i < turns; ++i)
            if (!actor_turn(alice, *a, shared, 1))
                workers_ok.store(false, std::memory_order_relaxed);
    });
    std::thread worker_b([&] {
        for (int i = 0; i < turns; ++i)
            if (!actor_turn(bob, *b, shared, 3))
                workers_ok.store(false, std::memory_order_relaxed);
    });
    worker_a.join();
    worker_b.join();
    require(workers_ok.load(std::memory_order_relaxed),
            "same executable callback failed on independent actors");
    require(alice.read<std::int64_t>(*a) == 10 + turns,
            "alice state was lost or unexpectedly shared");
    require(bob.read<std::int64_t>(*b) == 100 + 3 * turns,
            "bob state was lost or unexpectedly shared");

    std::int64_t supervisor_control = 7;
    supervisor_control = shared(supervisor_control, 5);
    require(supervisor_control == 12,
            "supervisor did not reuse the shared implementation");
    require(alice.read<std::int64_t>(*a) == 10 + turns
            && bob.read<std::int64_t>(*b) == 100 + 3 * turns,
            "supervisor bookkeeping mutated a child actor");
    require(alice.live_allocations() == 1 && bob.live_allocations() == 1,
            "independent actors did not retain their private allocations");

    // Exchanging handles is NOT an ownership transfer or shared-memory loan.
    require(!alice.read<std::int64_t>(*b) && !bob.read<std::int64_t>(*a),
            "foreign actor object handle was readable");
    require(!alice.write(*b, std::int64_t{999})
            && !bob.write(*a, std::int64_t{999}),
            "foreign actor object handle was writable");
    require(!alice.remove(*b) && !bob.remove(*a),
            "foreign actor was allowed to destroy another actor's object");
    require(alice.read<std::int64_t>(*a) == 10 + turns
            && bob.read<std::int64_t>(*b) == 100 + 3 * turns,
            "rejected foreign access modified private actor state");

    // A supervisor-directed replacement actor may reuse the logical slot,
    // but a new generation must never inherit an old private allocation.
    Heap replacement(ActorHeapIdentity{902, 21, 2});
    const auto new_state = replacement.allocate(sizeof(std::int64_t),
                                                alignof(std::int64_t));
    require(new_state.has_value(), "replacement actor allocation failed");
    require(!replacement.read<std::int64_t>(*a)
            && !alice.read<std::int64_t>(*new_state),
            "generation change leaked private objects");
    require(replacement.write(*new_state, std::int64_t{2}),
            "replacement actor initialization failed");
    require(actor_turn(replacement, *new_state, shared, 9)
            && replacement.read<std::int64_t>(*new_state) == 11,
            "replacement actor could not use the same implementation");

    alice.retire_quiesced(); // caller has already joined the worker above
    require(!alice.read<std::int64_t>(*a) && !alice.allocate(8),
            "retired actor exposed its former objects");
    require(bob.read<std::int64_t>(*b) == 100 + 3 * turns
            && replacement.read<std::int64_t>(*new_state) == 11
            && supervisor_control == 12,
            "retiring one actor damaged shared code users or supervisor state");
}

int main() {
    try {
        test_shared_code_with_independent_actor_and_supervisor_state();
        std::cout << "PASS host shared code with isolated actor/supervisor state\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
