#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

extern "C" {
void oreslang_actor_Counter_init(OresActorI64Heap*, std::uint64_t, std::uint64_t);
std::int64_t oreslang_actor_Counter_on_message(
    OresActorI64Heap*, std::uint64_t, std::uint64_t, std::int64_t);
}

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "COMPILED_ACTOR_STACKLESS_M3_FAILED %s:%d: %s\n", \
                 __FILE__, __LINE__, #expr); std::abort(); \
} } while (false)

struct Shared {
    MultiActorCarrier::ActorToken a{}, b{};
    std::atomic<int> errors{0};
    std::atomic<int> a_cleanup{0}, b_cleanup{0};
};

struct Actor {
    Shared* shared = nullptr;
    MultiActorCarrier::ActorToken owner{};
    OresActorI64Heap* heap = nullptr;
    std::atomic<std::int64_t> last{0};
    std::atomic<std::uint64_t> done{0};
    bool is_a = false;
};

static MultiActorCarrier::Outcome compiled_actor(
        void* raw, MultiActorCarrier::Message message,
        MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& actor = *static_cast<Actor*>(raw);
    auto& shared = *actor.shared;
    const auto other = actor.is_a ? shared.b : shared.a;
    MultiActorCarrier::GuestFrameToken foreign{};
    if (cause != MultiActorCarrier::Cause::initial || fuel != 32 ||
            message.sequence != actor.done.load(std::memory_order_relaxed) + 1 ||
            MultiActorCarrier::guest_frame_depth(actor.owner) != 0 ||
            MultiActorCarrier::guest_frame_depth(other) != 0 ||
            MultiActorCarrier::guest_frame_push(other, 8, foreign) ||
            foreign.serial != 0)
        shared.errors.fetch_add(1, std::memory_order_relaxed);

    // The real compiled AttoLang handler executes fib(n) by running immutable
    // compiler bytecode on actor-owned PC/operand/call frames. The helper
    // recursion never consumes native C++ call-stack frames.
    const auto turn = ores_actor_i64_heap_enter(
        actor.heap, actor.owner.slot, actor.owner.generation);
    const auto value = oreslang_actor_Counter_on_message(
        actor.heap, turn.domain, turn.epoch, message.payload);
    ores_actor_i64_heap_leave(actor.heap, turn);
    if (MultiActorCarrier::guest_frame_depth(actor.owner) != 0)
        shared.errors.fetch_add(1, std::memory_order_relaxed);
    actor.last.store(value, std::memory_order_release);
    actor.done.fetch_add(1, std::memory_order_release);
    return MultiActorCarrier::Outcome::complete;
}

static void cancel_actor(
        void* raw, MultiActorCarrier::Message, bool had_active) noexcept {
    auto& actor = *static_cast<Actor*>(raw);
    if (had_active || MultiActorCarrier::guest_frame_depth(actor.owner) != 0)
        actor.shared->errors.fetch_add(1, std::memory_order_relaxed);
    (actor.is_a ? actor.shared->a_cleanup : actor.shared->b_cleanup)
        .fetch_add(1, std::memory_order_release);
}

template<class Condition>
static bool until(Condition condition) {
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

static void init_heap(Actor& actor) {
    actor.heap = ores_actor_i64_heap_open(
        actor.owner.slot, actor.owner.generation, 1);
    REQUIRE(actor.heap != nullptr);
    const auto turn = ores_actor_i64_heap_enter(
        actor.heap, actor.owner.slot, actor.owner.generation);
    oreslang_actor_Counter_init(actor.heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(actor.heap, turn);
}
static void physically_reclaim(Actor& actor) {
    REQUIRE(actor.heap != nullptr);
    ores_actor_i64_heap_retire(actor.heap);
    REQUIRE(ores_actor_i64_heap_try_destroy(actor.heap) == 1);
    actor.heap = nullptr;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--overflow") == 0) {
        // Runtime negative: recursion must exhaust the logical frame budget,
        // not grow the carrier C stack. A successful return is a TEST FAILURE.
        Shared overflow_shared;
        Actor overflow_actor{};
        overflow_actor.shared = &overflow_shared;
        MultiActorCarrier overflow_carrier(32);
        overflow_actor.owner = overflow_carrier.register_actor(
            &overflow_actor, compiled_actor, cancel_actor);
        overflow_shared.a = overflow_actor.owner;
        overflow_shared.b = overflow_actor.owner;
        init_heap(overflow_actor);
        auto producer = overflow_carrier.producer();
        overflow_carrier.start();
        REQUIRE(producer.try_send(overflow_actor.owner, {1, 48}));
        // Expected: the worker terminates this process via std::abort() when
        // 32 actor-owned logical frames are exhausted. If it returns, fail.
        (void)until([&] { return overflow_actor.done.load() != 0; });
        std::fputs("FAIL: unbounded recursive actor handler returned\n", stderr);
        return 77;
    }
    REQUIRE(argc == 1);
    Shared shared;
    Actor a{}, b{};
    a.shared = &shared;
    b.shared = &shared;
    a.is_a = true;
    MultiActorCarrier carrier(32);
    a.owner = carrier.register_actor(&a, compiled_actor, cancel_actor);
    b.owner = carrier.register_actor(&b, compiled_actor, cancel_actor);
    shared.a = a.owner;
    shared.b = b.owner;
    REQUIRE(a.owner.generation != b.owner.generation);
    REQUIRE(a.owner.slot != b.owner.slot);
    init_heap(a);
    init_heap(b);

    // The imported native frame ABI is inaccessible off an actor carrier.
    MultiActorCarrier::GuestFrameToken off_carrier{};
    REQUIRE(!MultiActorCarrier::guest_frame_push(a.owner, 8, off_carrier));
    REQUIRE(!MultiActorCarrier::guest_frame_push(b.owner, 8, off_carrier));

    auto sender = carrier.producer();
    carrier.start();
    REQUIRE(sender.try_send(a.owner, {1, 6}));
    REQUIRE(sender.try_send(b.owner, {1, 7}));
    REQUIRE(until([&] {
        return a.done.load(std::memory_order_acquire) == 1 &&
               b.done.load(std::memory_order_acquire) == 1;
    }));
    REQUIRE(a.last.load() == 8 && b.last.load() == 13);
    REQUIRE(shared.errors.load() == 0);

    // STOP A and physically free A's heap while B's actor stack and heap
    // remain live and usable. The callbacks share one physical worker.
    REQUIRE(sender.request_cancel(a.owner));
    REQUIRE(until([&] { return carrier.actor_stats(a.owner).retired; }));
    REQUIRE(shared.a_cleanup.load() == 1);
    physically_reclaim(a);
    REQUIRE(!sender.try_send(a.owner, {2, 999}));
    REQUIRE(sender.try_send(b.owner, {2, 8}));
    REQUIRE(until([&] { return b.done.load() == 2; }));
    REQUIRE(b.last.load() == 21);
    REQUIRE(sender.try_send(b.owner, {3, 9}));
    REQUIRE(until([&] { return b.done.load() == 3; }));
    REQUIRE(b.last.load() == 34);

    REQUIRE(sender.request_cancel(b.owner));
    REQUIRE(until([&] { return carrier.actor_stats(b.owner).retired; }));
    REQUIRE(shared.b_cleanup.load() == 1);
    physically_reclaim(b);
    carrier.join();
    const auto total = carrier.stats();
    REQUIRE(total.accepted == 4);
    REQUIRE(total.completed == 4);
    REQUIRE(total.abandoned == 0);
    REQUIRE(total.protocol_failures == 0);
    REQUIRE(total.dispatches == 4);
    REQUIRE(shared.errors.load() == 0);
    REQUIRE(!sender.try_send(b.owner, {4, 777}));
    std::puts("ATTO_LLVM_COMPILED_STACKLESS_ACTOR_M3_PASS");
}
