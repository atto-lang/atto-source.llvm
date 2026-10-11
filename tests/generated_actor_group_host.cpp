#include "oreslang/runtime/GeneratedActorFieldWorker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>

using oreslang::runtime::GeneratedActorFieldWorker;
using oreslang::runtime::NativeActorGroup;
using namespace std::chrono_literals;

extern "C" {
void* ores_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
int64_t ores_actor_Counter_add(OresActorI64Heap*, uint64_t, uint64_t, void*, int64_t);
void ores_actor_Counter_drop(OresActorI64Heap*, uint64_t, uint64_t, void*);
}

static void require(bool test, const char* why) {
    if (!test) throw std::runtime_error(why);
}
static GeneratedActorFieldWorker make_unused(); // no implicit actor cloning

int main() {
    {
        NativeActorGroup group(100, 2);
        GeneratedActorFieldWorker worker(group, 100, 51, 3, 16,
                                         ores_actor_Counter_init, ores_actor_Counter_drop);
        auto ref = worker.ref();
        require(ref.get_ready().wait_for(0ms) == std::future_status::timeout,
                "spawn without await must not claim ready");
        require(group.worker_count() == 1, "group did not own managed actor");
        require(worker.initialize(), "generated actor initialization failed");
        ref.get_ready().get();
        require(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                "readiness must not mean physical termination");
        require(worker.invoke(ores_actor_Counter_add, int64_t{20}) == 32,
                "generated field record was not initialized");
        int64_t next = -1;
        std::thread migrated([&] {
            next = worker.invoke(ores_actor_Counter_add, int64_t{10});
        });
        migrated.join();
        require(next == 42, "persistent fields lost across carrier migration");
        group.close();
        require(worker.stop_requested(), "ActorGroup did not stop managed native actor");
        require(worker.poll_finalized(), "field record did not retire");
        ref.get_done().get();
        require(group.worker_count() == 0, "ActorGroup retained physically freed actor");
        require(worker.poll_finalized(), "duplicate terminal poll not idempotent");
    }
    {
        NativeActorGroup group(101, 1);
        GeneratedActorFieldWorker worker(group, 101, 52, 1, 16,
                                         ores_actor_Counter_init, ores_actor_Counter_drop);
        auto ref = worker.ref();
        require(worker.initialize(), "second generated actor not ready");
        // Stand-in for a suspended/FFI loan: the trusted runtime admits a
        // turn and pins native actor storage, then releases the carrier.
        OresActorI64Heap* heap = worker.trusted_heap();
        OresActorTurn turn = ores_actor_i64_heap_enter(heap, 52, 1);
        OresActorSlot slot = ores_actor_i64_heap_new(heap, turn, 900);
        OresActorLoan loan = ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_leave(heap, turn);
        group.stop_workers();
        require(!worker.poll_finalized(), "native heap freed during suspended loan");
        require(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                "done settled before loan physically released");
        ores_actor_i64_heap_release(heap, loan);
        require(worker.poll_finalized(), "final loan did not permit record cleanup");
        ref.get_done().get();
    }
    {
        NativeActorGroup group(102, 1);
        GeneratedActorFieldWorker worker(group, 102, 53, 1, 16,
                                         ores_actor_Counter_init, ores_actor_Counter_drop);
        auto ref = worker.ref();
        require(worker.initialize(), "cancellable actor not ready");
        worker.request_cancel();
        require(worker.poll_finalized(), "cancel did not finalize guest fields");
        ref.get_ready().get();
        bool cancelled = false;
        try { ref.get_done().get(); }
        catch (const std::runtime_error&) { cancelled = true; }
        require(cancelled, "cancel must settle terminal future with failure");
    }
    {
        NativeActorGroup group(103, 1);
        GeneratedActorFieldWorker worker(group, 103, 54, 1, 16,
                                         ores_actor_Counter_init, ores_actor_Counter_drop);
        auto ref = worker.ref();
        require(worker.initialize(), "panic actor not ready");
        bool caught = false;
        try {
            worker.invoke([](OresActorI64Heap*, uint64_t, uint64_t, void*, int64_t) -> int64_t {
                throw std::runtime_error("host-side generated handler failure");
            }, int64_t{7});
        } catch (const std::runtime_error&) {
            caught = true;
        }
        require(caught && worker.stop_requested(), "actor failure did not request teardown");
        require(worker.poll_finalized(), "panicking actor heap was not cleaned");
        bool failed = false;
        try { ref.get_done().get(); }
        catch (const std::runtime_error&) { failed = true; }
        require(failed, "panic not propagated to termination future");
    }
    {
        NativeActorGroup group(104, 1);
        GeneratedActorFieldWorker worker(group, 104, 55, 1, 16,
                                         ores_actor_Counter_init, ores_actor_Counter_drop);
        auto ref = worker.ref();
        group.close();
        require(!worker.initialize(), "closed group admitted late actor initialization");
        require(worker.poll_finalized(), "cancel-before-initialization leaked heap");
        bool ready_failed = false, done_failed = false;
        try { ref.get_ready().get(); } catch (const std::runtime_error&) { ready_failed = true; }
        try { ref.get_done().get(); } catch (const std::runtime_error&) { done_failed = true; }
        require(ready_failed && done_failed, "startup cancellation must fail both futures");
    }
    return 0;
}
