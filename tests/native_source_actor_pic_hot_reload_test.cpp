// Host-only AOT/PIC integration: REAL compiler-generated Oreslang actor
// v1/v2 call targets, one native heap, and NativeActorHotUpgradeGate pins.
// This is NOT a production authenticated loader, source scheduler, ORC JIT,
// sandbox, or general schema migration implementation. A trusted test harness
// builds both shared objects in a private directory before invocation.
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using oreslang::runtime::ActorHeapIdentity;
using Init = void (*)(OresActorI64Heap*, std::uint64_t, std::uint64_t);
using Receive = std::int64_t (*)(OresActorI64Heap*, std::uint64_t,
                                 std::uint64_t, std::int64_t);

struct Image final {
    Init init = nullptr;
    Receive receive = nullptr;
};
static_assert(std::is_nothrow_move_constructible<Image>::value,
              "an image must be movable without exceptions");
using Gate = oreslang::runtime::NativeActorHotUpgradeGate<Image>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> bool rejects(Action&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}
template<class FunctionPointer>
FunctionPointer find_symbol(void* library, const char* name) {
    static_assert(sizeof(FunctionPointer) == sizeof(void*),
                  "unsupported POSIX dynamic symbol ABI");
    dlerror();
    void* address = dlsym(library, name);
    const char* failure = dlerror();
    if (failure || !address)
        throw std::runtime_error(std::string("missing generated symbol: ") + name);
    FunctionPointer result{};
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
struct Loaded final {
    void* handle = nullptr;
    Image image{};
};
Loaded load_fixture(const char* path) {
    // Authentication of the artifact and transitive imports MUST happen
    // independently before this function is called in a production loader.
    // ELF constructors may execute inside dlopen: NEVER use this to verify
    // untrusted images after the fact. Only private test-built paths are used.
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        throw std::runtime_error(std::string("dlopen failed: ") + dlerror());
    try {
        Image image{
            find_symbol<Init>(handle, "oreslang_actor_Counter_init"),
            find_symbol<Receive>(handle, "oreslang_actor_Counter_on_message")
        };
        return {handle, image};
    } catch (...) {
        (void)dlclose(handle);
        throw;
    }
}
Gate::Shape shape() {
    return {Gate::Kind::trusted_shared, "Counter<int>:v1",
            "base:int;delta:int;total:int", "no-shared-domain",
            "trusted-test-send"};
}
std::int64_t receive(Gate& gate, ActorHeapIdentity id,
                     OresActorI64Heap* heap, std::int64_t n) {
    auto pin = gate.pin(id);
    auto turn = ores_actor_i64_heap_enter(heap, id.actor, id.epoch);
    const std::int64_t result = pin.code().receive(
        heap, turn.domain, turn.epoch, n);
    ores_actor_i64_heap_leave(heap, turn);
    return result;
}
struct HostMailbox {
    Gate* gate;
    OresActorI64Heap* heap;
    ActorHeapIdentity id;
    std::atomic<std::int64_t> last{0};
    std::atomic<unsigned> completed{0};
};
void source_actor_dispatch(void* raw, std::size_t actor, std::uint64_t payload) {
    auto* host = static_cast<HostMailbox*>(raw);
    if (!host || actor != 0) std::abort();
    try {
        auto value = receive(*host->gate, host->id, host->heap,
                             static_cast<std::int64_t>(payload));
        host->last.store(value, std::memory_order_relaxed);
        host->completed.fetch_add(1, std::memory_order_release);
    } catch (...) {
        // A C native scheduler callback cannot unwind into its carrier.
        std::abort();
    }
}
void wait_mail(HostMailbox& host, unsigned count, std::int64_t expected) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (host.completed.load(std::memory_order_acquire) < count
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(host.completed.load(std::memory_order_acquire) == count,
            "source actor mailbox callback missing or duplicated");
    require(host.last.load(std::memory_order_relaxed) == expected,
            "source actor compiled callback result differs");
}
void wait_mailbox_quiescence(OresNativeActorScheduler* scheduler,
                             OresNativeActorRef actor, std::uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (!ores_native_actor_scheduler_upgrade_quiesced(scheduler, actor, ticket)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(ores_native_actor_scheduler_upgrade_quiesced(scheduler, actor, ticket),
            "accepted mailbox work did not reach scheduler quiescence");
}
void unload(void* handle, std::atomic<int>& count) {
    // Test-only synchronous close. Production cleanup MUST be queued outside
    // CONTROL and must durably quarantine any failed dlclose/ORC teardown.
    if (dlclose(handle) != 0)
        throw std::runtime_error("test fixture shared-library unload failed");
    ++count;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: source-actor-pic-gate <v1.so> <v2.so>\n";
        return 2;
    }
    try {
        const ActorHeapIdentity id{101, 81, 1};
        std::atomic<int> first_unloaded{0};
        std::atomic<int> second_unloaded{0};
        std::atomic<int> rejected_unloaded{0};
        std::atomic<int> busy_unloaded{0};
        const auto initial = load_fixture(argv[1]);
        OresActorI64Heap* const heap = ores_actor_i64_heap_open(
            id.actor, id.epoch, 3);
        Gate gate(id, shape(), initial.image,
                  [handle = initial.handle, &first_unloaded] {
                      unload(handle, first_unloaded);
                  });
        // Even first-generation initialization is guest executable code.
        // Register a generation lease BEFORE invoking the compiler-emitted
        // init function so future loader integration cannot call into
        // unpinned executable pages while a concurrent retire is possible.
        {
            auto initialized = gate.pin(id);
            const auto init_turn = ores_actor_i64_heap_enter(
                heap, id.actor, id.epoch);
            initialized.code().init(heap, init_turn.domain, init_turn.epoch);
            ores_actor_i64_heap_leave(heap, init_turn);
        }
        require(receive(gate, id, heap, 0) == 42,
                "compiler-generated v1 did not produce 42");
        require(rejects([&] {
            (void)gate.pin(ActorHeapIdentity{id.runtime, id.actor + 1, id.epoch});
        }), "foreign actor identity was allowed to pin executable code");

        // Incompatible ABI must not publish, mutate state, or unload old code.
        {
            const auto incompatible = load_fixture(argv[2]);
            auto ticket = gate.freeze(id, 1);
            auto wrong = shape();
            wrong.state_schema = "incompatible-field-layout";
            require(rejects([&] {
                (void)gate.promote(ticket, wrong, incompatible.image,
                    [handle = incompatible.handle, &rejected_unloaded] {
                        unload(handle, rejected_unloaded);
                    });
            }), "incompatible field ABI published");
            require(first_unloaded == 0 && rejected_unloaded == 1,
                    "ABI refusal leaked/replaced a live image");
        }

        const auto next = load_fixture(argv[2]);
        {
            auto suspended = gate.pin(id); // Simulates an awaiting old frame.
            auto ticket = gate.freeze(id, 1);
            require(!gate.quiesced(ticket),
                    "awaiting old code must block quiescence");
            require(rejects([&] { (void)gate.pin(id); }),
                    "frozen DATA admission permitted a new turn");
            // A false quiescence observation is not enough: attempt the
            // forbidden publication and prove both the old image and
            // replacement-resource ownership survive the refusal.
            const auto busy = load_fixture(argv[2]);
            require(rejects([&] {
                (void)gate.promote(ticket, shape(), busy.image,
                    [handle = busy.handle, &busy_unloaded] {
                        unload(handle, busy_unloaded);
                    });
            }), "busy old code was replaced while pinned");
            require(busy_unloaded == 1 && first_unloaded == 0,
                    "busy upgrade leaked candidate or reclaimed live code");
            require(suspended.code().receive != nullptr,
                    "old pinned executable image became invalid");
            suspended.close();
            require(gate.quiesced(ticket), "released code pin remained active");
            const auto result = gate.promote(ticket, shape(), next.image,
                [handle = next.handle, &second_unloaded] {
                    unload(handle, second_unloaded);
                });
            require(result.revision == 2, "revision did not advance");
        }
        require(first_unloaded == 1,
                "old compiled DSO not reclaimed after its final lease");
        require(receive(gate, id, heap, 1) == 86,
                "v2 code did not run on preserved actor heap");
        require(receive(gate, id, heap, 0) == 128,
                "v2 subsequent turn lost persistent actor state");
        require(rejects([&] { (void)gate.freeze(id, 1); }),
                "stale supervisor revision admitted");

        // Persistent actor heap is retired independently from code pages.
        gate.close();
        require(second_unloaded == 1,
                "new DSO did not retire on terminal code close");
        require(rejects([&] { (void)gate.pin(id); }),
                "closed actor accepted a fresh code-generation lease");
        ores_actor_i64_heap_retire(heap);
        require(ores_actor_i64_heap_try_destroy(heap) == 1,
                "native actor-owned heap did not quiesce/reclaim");
        std::cout << "ACTUAL_ORES_SOURCE_AOT_PIC_V1_V2_PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "AOT/PIC source actor test failed: " << failure.what() << '\n';
        return 1;
    }
}
