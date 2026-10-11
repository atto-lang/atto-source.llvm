#include "oreslang/runtime/ActorGroupOwnedHeap.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>

extern "C" {
OresActorI64Fields* ores_actor_Counter_init(OresActorI64Heap*, std::uint64_t, std::uint64_t);
std::uint64_t ores_actor_Counter_layout_hash();
std::uint32_t ores_actor_Counter_layout_version();
std::int64_t ores_actor_Counter_add(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                                    OresActorI64Fields*, std::int64_t);
void ores_actor_Counter_drop(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                              OresActorI64Fields*);
}

using oreslang::runtime::NativeActorExit;
using oreslang::runtime::NativeActorFieldEntrypoints;
using oreslang::runtime::NativeActorOwnedHeapGroup;
using namespace std::chrono_literals;

static void check(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

static bool rejected(std::shared_future<void> future) {
    try { future.get(); }
    catch (const std::runtime_error&) { return true; }
    return false;
}

static OresActorI64Fields* failed_init(OresActorI64Heap*, std::uint64_t, std::uint64_t) {
    throw std::runtime_error("simulated on_start failure");
}

int main() {
    try {
        std::atomic<int> queued{0};
        constexpr std::uint64_t shared_java_llvm_expected = UINT64_C(0xb2d57409773a17ed);
        NativeActorFieldEntrypoints generated{
            &ores_actor_Counter_init, &ores_actor_Counter_drop,
            &ores_actor_Counter_layout_hash, &ores_actor_Counter_layout_version,
            2, shared_java_llvm_expected
        };
        check(ores_actor_Counter_layout_hash() == shared_java_llvm_expected,
              "LLVM layout hash diverges from Java/Graal v1 contract");
        check(ores_actor_Counter_layout_version() == 1, "wrong LLVM layout revision");
        {
            NativeActorOwnedHeapGroup wrong_group(510, 1);
            auto broken = generated;
            broken.expected_layout_hash ^= 1;
            bool blocked = false;
            try {
                (void) wrong_group.spawn(9, 1, 16, broken, [&] { ++queued; });
            } catch (const std::invalid_argument&) { blocked = true; }
            check(blocked, "mismatched Java/LLVM actor layout was admitted");
            check(wrong_group.lifecycle().worker_count() == 0,
                  "rejected layout left an actor registered");
        }
        {
            NativeActorOwnedHeapGroup group(511, 4);
            auto worker = group.spawn(11, 3, 16, generated, [&] { ++queued; });
            auto ref = worker->ref();
            check(ref.get_ready().wait_for(0ms) == std::future_status::timeout,
                  "unawaited spawn must not have completed initialization");
            check(worker->initialize(), "generated actor initialization failed");
            ref.get_ready().get();
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "ready must not imply done");
            std::int64_t answer = 0;
            check(worker->with_turn([&](auto* heap, OresActorTurn turn, auto* fields) {
                answer = ores_actor_Counter_add(
                    heap, turn.domain, turn.epoch, fields, 20);
            }) && answer == 32, "compiled first actor method failed");

            OresActorLoan loan{};
            std::optional<oreslang::runtime::NativeActorHeapWorker::ContinuationPin> continuation;
            check(worker->with_turn([&](auto* heap, OresActorTurn turn, auto* fields) {
                loan = ores_actor_i64_fields_borrow(heap, turn, fields, 0);
                continuation.emplace(worker->pin_continuation());
            }), "actor failed to pin field and await continuation");
            check(worker->pending_loans() == 1, "suspended field loan was not retained");
            check(worker->pending_continuations() == 1,
                  "await continuation escaped without a lifecycle pin");

            group.request_stop_all();
            check(!worker->with_turn([](auto*, auto, auto*) {}),
                  "cancelled actor accepted another guest turn");
            check(queued.load() >= 1, "stop request did not queue CONTROL");
            check(group.drain_control() == 0,
                  "CONTROL dropped a still-borrowed actor record");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "get_done completed before final physical release");

            std::thread foreign([worker, loan] { worker->release_field_loan(loan); });
            foreign.join();
            check(group.drain_control() == 0,
                  "CONTROL destroyed heap while await continuation still pinned");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "FFI completion thread incorrectly finalized actor");
            std::thread completion([&] { continuation.reset(); });
            completion.join();
            check(worker->pending_continuations() == 0,
                  "released await continuation was still charged");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "completion thread bypassed parent CONTROL lane");
            check(group.drain_control() == 1,
                  "supervisor CONTROL failed to drain now-quiescent actor");
            check(worker->physically_finalized(), "actor memory was not freed");
            check(rejected(ref.get_done()), "cancelled actor had successful done");
            check(group.drain_control() == 0, "duplicate finalization succeeded");
        }
        {
            NativeActorOwnedHeapGroup group(512, 2);
            auto worker = group.spawn(21, 5, 16, generated, [&] { ++queued; });
            check(worker->initialize(), "normal actor initializer failed");
            auto ref = worker->ref();
            std::thread second_carrier([worker] {
                bool success = worker->with_turn([](auto* heap, auto turn, auto* fields) {
                    if (ores_actor_Counter_add(heap, turn.domain, turn.epoch, fields, 10)
                            != 22)
                        throw std::runtime_error("cross-carrier field persistence failure");
                });
                if (!success) throw std::runtime_error("cross-carrier turn failed");
            });
            second_carrier.join();
            worker->request_normal_stop();
            check(group.drain_control() == 1, "normal termination did not finalize");
            ref.get_done().get();
            check(worker->physically_finalized(), "normal actor heap leaked");
        }
        {
            NativeActorOwnedHeapGroup group(513, 2);
            auto worker = group.spawn(31, 7, 16, generated, [&] { ++queued; });
            check(worker->initialize(), "panic actor initializer failed");
            auto ref = worker->ref();
            check(!worker->with_turn([](auto*, auto, auto*) {
                throw std::runtime_error("host handler panic");
            }), "handler panic was not captured");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "panic bypassed parent CONTROL finalization");
            check(group.drain_control() == 1, "panic was not cleaned up");
            check(rejected(ref.get_done()), "panic failed to propagate to done");
        }
        {
            NativeActorOwnedHeapGroup group(514, 2);
            auto broken = generated;
            broken.init = &failed_init;
            auto worker = group.spawn(41, 9, 16, broken, [&] { ++queued; });
            auto ref = worker->ref();
            check(!worker->initialize(), "throwing initializer was accepted");
            check(group.drain_control() == 1, "startup panic did not retire empty heap");
            check(rejected(ref.get_ready()), "on_start panic accepted ready");
            check(rejected(ref.get_done()), "on_start panic accepted done");
        }
        {
            NativeActorOwnedHeapGroup group(515, 2);
            auto worker = group.spawn(51, 11, 16, generated, [&] { ++queued; });
            auto ref = worker->ref();
            check(worker->initialize(), "close test failed init");
            OresActorLoan loan{};
            check(worker->with_turn([&](auto* heap, OresActorTurn turn, auto* fields) {
                loan = ores_actor_i64_fields_borrow(heap, turn, fields, 1);
            }), "close test loan creation failed");
            group.close();
            check(group.drain_control() == 0, "close freed active FFI loan");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "close falsely settled done");
            worker->release_field_loan(loan);
            check(group.drain_control() == 1,
                  "closed group rejected late quiescent finalization");
            check(rejected(ref.get_done()), "closed actor incorrectly completed normally");
        }
        {
            // Releasing a completed continuation inline on its own actor lane
            // must not reacquire lane_ or deadlock a physical carrier.
            NativeActorOwnedHeapGroup group(516, 1);
            auto worker = group.spawn(61, 1, 16, generated, [&] { ++queued; });
            check(worker->initialize(), "inline-continuation actor initialization failed");
            check(worker->with_turn([&](auto*, auto, auto*) {
                auto pin = worker->pin_continuation();
                check(worker->pending_continuations() == 1,
                      "inline continuation pin was not accounted");
                pin.release();
                check(worker->pending_continuations() == 0,
                      "inline continuation completion did not release pin");
            }), "inline continuation completion failed");
            worker->request_normal_stop();
            check(group.drain_control() == 1,
                  "inline completed continuation stranded actor finalization");
            worker->ref().get_done().get();
        }
        {
            // Parent CONTROL processing must not block behind an admitted
            // child handler: the child's completion enqueues another wake.
            NativeActorOwnedHeapGroup group(517, 1);
            auto worker = group.spawn(62, 1, 16, generated, [&] { ++queued; });
            check(worker->initialize(), "busy-turn actor initialization failed");
            std::promise<void> admitted;
            std::promise<void> finish_turn;
            auto release = finish_turn.get_future().share();
            std::atomic<bool> guest_ok{false};
            std::thread child([&] {
                guest_ok.store(worker->with_turn([&](auto*, auto, auto*) {
                    admitted.set_value();
                    if (release.wait_for(5s) != std::future_status::ready)
                        throw std::runtime_error("guest turn release timed out");
                }));
            });
            bool entered = admitted.get_future().wait_for(5s) == std::future_status::ready;
            // Stop after the actor has demonstrably entered the turn;
            // otherwise an early CONTROL poll proves nothing about blocking.
            if (entered) worker->request_normal_stop();
            std::promise<std::size_t> control_result;
            auto settled = control_result.get_future();
            std::thread parent([&] {
                try { control_result.set_value(group.drain_control()); }
                catch (...) { control_result.set_exception(std::current_exception()); }
            });
            auto responsiveness = settled.wait_for(500ms);
            finish_turn.set_value();
            child.join();
            parent.join();
            check(entered, "busy native actor never entered its turn");
            check(responsiveness == std::future_status::ready,
                  "parent CONTROL blocked a carrier behind guest actor");
            check(settled.get() == 0, "CONTROL finalized an active actor");
            check(guest_ok.load(), "running guest turn failed while stopping");
            check(group.drain_control() == 1,
                  "child completion did not enable pending CONTROL teardown");
            worker->ref().get_done().get();
        }
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "actor group test failed: %s\n", failure.what());
        return 63;
    }
}
