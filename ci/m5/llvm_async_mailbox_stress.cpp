// M5 real native LLVM TenantVmHost asynchronous queue/failure stress.
// Uses the existing C ABI test module: NOT itself a compiler-generated .atto
// program and NOT a proof of arbitrary hostile-native-code sandboxing.
#include "guests.h"
#include "oreslang/runtime/TenantVm.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime::vm;
using Bytes = std::vector<std::uint8_t>;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static Bytes word_payload(std::int64_t x) {
    Bytes v(sizeof(x));
    std::memcpy(v.data(), &x, sizeof(x));
    return v;
}
static Bytes pair_payload(std::int64_t a, std::int64_t b) {
    Bytes v(sizeof(a) + sizeof(b));
    std::memcpy(v.data(), &a, sizeof(a));
    std::memcpy(v.data() + sizeof(a), &b, sizeof(b));
    return v;
}
static std::int64_t result_i64(const TurnResult& result) {
    check(result.status == Status::Ok, "native actor call did not complete");
    check(result.payload.size() == 8, "actor reply is not one i64");
    std::int64_t r = 0;
    std::memcpy(&r, result.payload.data(), 8);
    return r;
}
static VmSpec spec(const std::string& id) {
    VmSpec s;
    s.tenant_id = id;
    s.module = &ores_vm_test_module;
    s.limits.mailbox_capacity = 8;
    s.limits.max_inflight = 32;
    s.limits.max_queued_bytes = 4096;
    s.limits.max_parallel_turns = 1;
    s.limits.turn_deadline_ms = 2000;
    return s;
}
int main() {
    try {
        HostLimits limits;
        limits.carriers = 4;
        TenantVmHost host(limits);
        auto a = host.open(spec("m5-worker-a"));
        auto b = host.open(spec("m5-neighbor-b"));
        check(a.status == Status::Ok && b.status == Status::Ok, "cannot open distinct tenants");
        auto actor = host.spawn_actor(a.handle, KIND_COUNTER);
        auto neighbor = host.spawn_actor(b.handle, KIND_COUNTER);
        check(actor.status == Status::Ok && neighbor.status == Status::Ok,
              "cannot spawn native actor workers");

        std::atomic<unsigned> completed{0};
        std::atomic<unsigned> full{0};
        std::atomic<unsigned> bad{0};
        auto produce = [&] {
            for (unsigned n = 0; n < 2000; ++n) {
                bool admitted = false;
                for (unsigned attempt = 0; attempt < 100000 && !admitted; ++attempt) {
                    auto response = host.call(actor.actor, word_payload(1)).get();
                    if (response.status == Status::MailboxFull ||
                            response.status == Status::Busy) {
                        full.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    } else if (response.status == Status::Ok) {
                        completed.fetch_add(1, std::memory_order_relaxed);
                        admitted = true;
                    } else {
                        bad.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
                if (!admitted) { bad.fetch_add(1, std::memory_order_relaxed); return; }
            }
        };
        std::thread p1(produce);
        std::thread p2(produce);
        std::thread p3(produce);
        std::thread p4(produce);
        for (int i = 0; i < 200; ++i) {
            check(result_i64(host.call(neighbor.actor, word_payload(2)).get()) ==
                  2 * (i + 1), "neighbor tenant lost progress during queue pressure");
        }
        p1.join(); p2.join(); p3.join(); p4.join();
        check(bad.load() == 0, "producer observed unexpected native runtime refusal/fault");
        check(completed.load() == 8000, "native MPSC actor lost or duplicated a turn");
        check(result_i64(host.call(actor.actor, word_payload(0)).get()) == 8000,
              "native actor state does not equal acknowledged turn count");
        check(result_i64(host.call(neighbor.actor, word_payload(0)).get()) == 400,
              "healthy neighboring tenant was starved or corrupted");

        // Native supervisor behavior after a deliberately trapped turn. This
        // validates host-visible typed failure + actor confinement, not a
        // guest-language supervisor CONTROL message channel yet.
        auto fault = host.spawn_actor(a.handle, KIND_RESTARTABLE);
        check(fault.status == Status::Ok, "cannot spawn fault-observable native actor");
        check(result_i64(host.call(fault.actor, pair_payload(RESTART_ADD, 10)).get()) == 10,
              "restart actor baseline state wrong");
        auto failed = host.call(fault.actor, pair_payload(RESTART_ADD_THEN_TRAP, 1000)).get();
        check(failed.status == Status::ActorRestarted && failed.failure == FailReason::Trap,
              "native actor trap was not routed to supervisor-visible restart");
        check(result_i64(host.call(fault.actor, pair_payload(RESTART_ADD, 1)).get()) == 11,
              "actor restart retained state from failed turn");
        check(result_i64(host.call(neighbor.actor, word_payload(0)).get()) == 400,
              "neighbor tenant failed after confined actor trap");

        check(host.stop_actor(fault.actor) == Status::Ok, "cannot retire failed actor");
        check(host.stop_actor(actor.actor) == Status::Ok, "cannot retire pressure actor");
        check(host.stop_actor(neighbor.actor) == Status::Ok, "cannot retire independent neighbor");
        check(host.begin_shutdown(a.handle) == Status::Ok, "cannot stop first tenant");
        check(host.begin_shutdown(b.handle) == Status::Ok, "cannot stop second tenant");
        auto closed_a = host.wait_closed(a.handle);
        auto closed_b = host.wait_closed(b.handle);
        check(closed_a.phase == Phase::Stopped && closed_b.phase == Phase::Stopped,
              "tenant failed deterministic shutdown");
        check(closed_a.actor_restarts == 1, "actor restart was not accounted");
        check(host.stats().queued_bytes == 0, "retired tenants left queued host-byte charges");

        std::puts("M5_LLVM_MPSC_ACCEPTED=8000");
        std::puts("M5_LLVM_NEIGHBOR_ACCEPTED=200");
        std::puts("M5_LLVM_NEIGHBOR_STATE=400");
        std::puts("M5_LLVM_ACTOR_STATE=8000");
        std::puts("M5_LLVM_ACTOR_RESTARTS=1");
        std::puts("M5_LLVM_UNEXPECTED_PRODUCER_ERRORS=0");
        std::puts("M5_LLVM_POST_DRAIN_QUEUED_BYTES=0");
        std::puts("M5_LLVM_NATIVE_ASYNC_MAILBOX_HOST=PASSED");
        std::puts("M5_SCOPE=NATIVE_TENANT_VM_HOST_C_ABI_NOT_COMPILED_ATTO_GUEST_OR_OS_SANDBOX");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "M5_LLVM_ASYNC_MAILBOX_FAILED: %s\n", e.what());
        return 1;
    }
}
