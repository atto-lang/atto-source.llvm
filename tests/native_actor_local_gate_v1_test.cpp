#include "oreslang/runtime/NativeActorLocalGateV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>

using namespace oreslang::runtime;

namespace {
ScopedLocalIdentityV1 identity(std::uint64_t generation) {
    return {5, 7, 9, 11, generation};
}

void real_native_execution_claims_gate_actor_local_access() {
    OresNativeExecutionAuthority authority;
    assert(ores_execution_init(&authority));

    ActorLocalStoreV1 first(identity(1));
    ActorLocalKeyV1<std::string> label;
    NativeActorLocalGateV1 gate(authority, first, identity(1));
    OresExecutionClaim turn{};
    assert(!gate.set(turn, label, std::string("not-admitted")));
    assert(ores_execution_enter(&authority, 1, 33, 0, &turn));
    assert(gate.authorized(turn));
    assert(gate.set(turn, label, std::string("first")));
    assert(gate.get_copy(turn, label) == std::string("first"));

    // A producer thread with a byte-for-byte copy of a true owner claim must
    // not gain actor-local reads/writes or revoke that actor's values.
    std::atomic<bool> foreign_denied{false};
    std::thread thief([&] {
        bool denied = !gate.authorized(turn)
            && !gate.get_copy(turn, label)
            && !gate.set(turn, label, std::string("stolen"))
            && !gate.remove(turn, label);
        foreign_denied.store(denied, std::memory_order_release);
    });
    thief.join();
    assert(foreign_denied.load(std::memory_order_acquire));
    assert(gate.get_copy(turn, label) == std::string("first"));

    assert(ores_execution_leave(&authority, turn, 1));
    assert(!gate.authorized(turn));
    assert(!gate.get_copy(turn, label));

    OresExecutionClaim resumed{};
    assert(ores_execution_enter(&authority, 1, 44, 0, &resumed));
    assert(resumed.sequence != turn.sequence);
    assert(!gate.get_copy(turn, label)); // prior physical claim is stale
    assert(gate.get_copy(resumed, label) == std::string("first"));
    assert(gate.remove(resumed, label));
    assert(!gate.get_copy(resumed, label));

    assert(ores_execution_stop(&authority, 1));
    assert(ores_execution_leave(&authority, resumed, 0));
    assert(ores_execution_retired(&authority, 1));
    assert(first.close()); // physical teardown before next incarnation
    assert(ores_execution_restart(&authority) == 2);
    OresExecutionClaim next{};
    assert(ores_execution_enter(&authority, 2, 45, 0, &next));
    assert(!gate.authorized(next)); // stale store cannot revive by ABA
    assert(!gate.set(next, label, std::string("wrong-generation")));

    ActorLocalStoreV1 replacement(identity(2));
    NativeActorLocalGateV1 next_gate(authority, replacement, identity(2));
    assert(next_gate.set(next, label, std::string("second")));
    assert(next_gate.get_copy(next, label) == std::string("second"));
    assert(!gate.get_copy(next, label));
    assert(ores_execution_leave(&authority, next, 0));
    assert(ores_execution_stop(&authority, 2));
    assert(ores_execution_retired(&authority, 2));
    assert(replacement.close());
    assert(ores_execution_destroy(&authority));
}
}

int main() {
    real_native_execution_claims_gate_actor_local_access();
    std::puts("NATIVE_ACTOR_LOCAL_GATE_HOST_ONLY_PASS");
}
