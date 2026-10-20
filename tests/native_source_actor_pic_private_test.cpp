// Source-generated trusted PRIVATE actor AOT/PIC test.
// A shared immutable code image must not imply a shared actor-owned heap.
// This is NOT an ORC JIT, sandbox, authenticated loader, or full scheduler test.
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using oreslang::runtime::ActorHeapIdentity;
using Init = void (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t);
using Receive = std::int64_t (*)(OresActorI64Heap*, std::uint64_t,
                                 std::uint64_t, std::int64_t);
struct Image { Init init; Receive receive; };
using Gate = oreslang::runtime::NativeActorHotUpgradeGate<Image>;

void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
template<class F> bool rejected(F&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}
template<class F> F symbol(void* handle, const char* name) {
    static_assert(sizeof(F) == sizeof(void*), "unsupported dlsym ABI");
    dlerror();
    void* raw = dlsym(handle, name);
    const char* error = dlerror();
    if (error || !raw) throw std::runtime_error(
        std::string("missing compiled source actor symbol: ") + name);
    F func{};
    std::memcpy(&func, &raw, sizeof(func));
    return func;
}
struct Library { void* handle; Image code; };
Library open(const char* filename) {
    void* handle = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(
        std::string("dlopen trusted fixture: ") + dlerror());
    try {
        return {handle, {symbol<Init>(handle, "oreslang_actor_Counter_init"),
                         symbol<Receive>(handle, "oreslang_actor_Counter_on_message")}};
    } catch (...) {
        (void)dlclose(handle);
        throw;
    }
}
Gate::Shape private_shape() {
    return {Gate::Kind::trusted_private, "Counter<int>:v1",
            "base:int;delta:int;total:int", "no-shared-domain",
            "trusted-test-send"};
}
void initialize(Gate& gate, ActorHeapIdentity id, OresActorI64Heap* heap) {
    auto pin = gate.pin(id);
    auto turn = ores_actor_i64_heap_enter(heap, id.actor, id.epoch);
    pin.code().init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);
}
std::int64_t receive(Gate& gate, ActorHeapIdentity id,
                     OresActorI64Heap* heap, std::int64_t input) {
    auto pin = gate.pin(id);
    auto turn = ores_actor_i64_heap_enter(heap, id.actor, id.epoch);
    auto result = pin.code().receive(heap, turn.domain, turn.epoch, input);
    ores_actor_i64_heap_leave(heap, turn);
    return result;
}
void retire_heap(OresActorI64Heap* heap) {
    ores_actor_i64_heap_retire(heap);
    require(ores_actor_i64_heap_try_destroy(heap) == 1,
            "actor heap did not retire independently");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: private-source-actor-pic <v1.so> <v2.so>\n";
        return 2;
    }
    try {
        // Loading the same DSO twice yields two references to one immutable
        // image, while gate/heap/actor identities must remain independent.
        auto v1a = open(argv[1]);
        auto v1b = open(argv[1]);
        const ActorHeapIdentity a{101, 701, 1};
        const ActorHeapIdentity b{101, 702, 1};
        OresActorI64Heap* heap_a = ores_actor_i64_heap_open(a.actor, a.epoch, 3);
        OresActorI64Heap* heap_b = ores_actor_i64_heap_open(b.actor, b.epoch, 3);
        require(heap_a && heap_b && heap_a != heap_b,
                "private actors unexpectedly share one heap");
        Gate gate_a(a, private_shape(), v1a.code,
                    [handle=v1a.handle] { require(dlclose(handle) == 0,
                                           "v1 A reclaim failed"); });
        Gate gate_b(b, private_shape(), v1b.code,
                    [handle=v1b.handle] { require(dlclose(handle) == 0,
                                           "v1 B reclaim failed"); });
        initialize(gate_a, a, heap_a);
        initialize(gate_b, b, heap_b);
        require(receive(gate_a, a, heap_a, 0) == 42,
                "private A v1 source handler incorrect");
        require(receive(gate_b, b, heap_b, 5) == 47,
                "private B lost independent initial state");
        require(rejected([&] { (void)gate_a.pin(b); })
                && rejected([&] { (void)gate_b.pin(a); }),
                "private actor accepted foreign heap/code pin");

        auto next_a = open(argv[2]);
        auto ticket = gate_a.freeze(a, 1);
        require(rejected([&] { (void)gate_a.pin(a); }),
                "frozen A accepted new DATA pin");
        // Freeze/replacement of A must neither freeze B nor alter B's data.
        require(receive(gate_b, b, heap_b, -1) == 88,
                "A upgrade interfered with independent B");
        require(gate_a.quiesced(ticket), "private actor A not quiescent");
        auto promoted = gate_a.promote(ticket, private_shape(), next_a.code,
                    [handle=next_a.handle] { require(dlclose(handle) == 0,
                                           "v2 A reclaim failed"); });
        require(promoted.revision == 2, "private A code revision did not advance");
        require(receive(gate_a, a, heap_a, 1) == 86,
                "private A source v2 or persistent heap state incorrect");
        require(receive(gate_b, b, heap_b, 0) == 130,
                "B incorrectly switched compiled handler with A");
        require(receive(gate_a, a, heap_a, 0) == 128,
                "private A restarted or lost state on code upgrade");
        require(rejected([&] { (void)gate_a.freeze(a, 1); }),
                "stale private A supervisor revision was accepted");
        gate_a.close();
        retire_heap(heap_a);
        require(receive(gate_b, b, heap_b, 0) == 172,
                "closing A invalidated still-running private B");
        gate_b.close();
        retire_heap(heap_b);
        std::cout << "ACTUAL_ORES_SOURCE_PRIVATE_AOT_PIC_ISOLATION_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "private source PIC hot reload FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
