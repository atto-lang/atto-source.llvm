#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/PinnedContinuationGate.hpp"
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using oreslang::runtime::AsyncIoTerminal;
using oreslang::runtime::AsyncIoTicket;
using oreslang::runtime::PinnedContinuationGate;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Isolate fatal-contract tests: the heap ABI must fail closed, not silently
// allow writes to immutable fields or through a suspended shared loan.
static void expect_field_write_abort(OresActorI64Heap* heap,
                                     std::uint64_t actor, std::uint64_t index) {
    const pid_t child = fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        struct rlimit limit = {0, 0};
        (void) setrlimit(RLIMIT_CORE, &limit);
        const OresActorTurn turn = ores_actor_i64_heap_enter(heap, actor, 1);
        ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, index, 999);
        _exit(0); // MUST remain unreachable
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "waitpid failed");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "forbidden actor field mutation did not fail closed");
}

int main() {
    for (std::uint64_t round = 1; round <= 128; ++round) {
        const std::uint64_t actor = round + 400;
        OresActorI64Heap* heap = ores_actor_i64_heap_open(actor, 1, 2);
        const OresActorTurn init = ores_actor_i64_heap_enter(heap, actor, 1);
        ores_actor_i64_heap_field_init_mutable(heap, init.domain, init.epoch, 0, 17);
        ores_actor_i64_heap_field_init(heap, init.domain, init.epoch, 1, 5);
        const OresActorSlot slot = {init.domain, 0, 1};
        const OresActorLoan loan = ores_actor_i64_heap_borrow(heap, init, slot);
        ores_actor_i64_heap_leave(heap, init);

        // An actor's shared loan survives physical carrier turnover, but it
        // cannot be dereferenced or mutated by the asynchronous I/O worker.
        const AsyncIoTicket ticket{actor, 1, round};
        PinnedContinuationGate frame(ticket);
        check(frame.acquire(ticket, PinnedContinuationGate::Loan::shared),
              "pinned continuation failed to acquire matching loan");
        check(!frame.try_retire(ticket), "pending actor frame retired");
        const OresActorTurn resumedTurn = ores_actor_i64_heap_enter(heap, actor, 1);
        check(ores_actor_i64_heap_field_read(heap, resumedTurn.domain,
                resumedTurn.epoch, 0) == 17, "persistent field changed across turn");
        check(ores_actor_i64_heap_field_read(heap, resumedTurn.domain,
                resumedTurn.epoch, 1) == 5, "immutable field changed across turn");
        ores_actor_i64_heap_leave(heap, resumedTurn);

        if (round == 1) {
            expect_field_write_abort(heap, actor, 0);
            expect_field_write_abort(heap, actor, 1);
        }

        // Ownership isolation: a separate actor can mutate its own field
        // while the first actor's shared loan remains live and suspended.
        OresActorI64Heap* other = ores_actor_i64_heap_open(actor + 100000, 1, 1);
        OresActorTurn o = ores_actor_i64_heap_enter(other, actor + 100000, 1);
        ores_actor_i64_heap_field_init_mutable(other, o.domain, o.epoch, 0, 3);
        ores_actor_i64_heap_leave(other, o);
        o = ores_actor_i64_heap_enter(other, actor + 100000, 1);
        ores_actor_i64_heap_field_write(other, o.domain, o.epoch, 0, 31);
        check(ores_actor_i64_heap_field_read(other, o.domain, o.epoch, 0) == 31,
              "unrelated actor mutation failed");
        ores_actor_i64_heap_leave(other, o);
        ores_actor_i64_heap_retire(other);
        check(ores_actor_i64_heap_try_destroy(other), "unrelated actor heap not reclaimed");

        std::thread broker([&] {
            check(frame.settle(ticket, AsyncIoTerminal::cancelled), "terminal cancellation lost");
            check(frame.broker_acknowledge(ticket), "broker quiescence ACK missing");
        });
        broker.join();
        check(!frame.settle(ticket, AsyncIoTerminal::success),
              "late success overwrote cancellation");
        check(frame.try_claim_resume(ticket) && !frame.try_claim_resume(ticket),
              "cancellation cleanup resumed twice");
        check(!frame.try_retire(ticket), "unfinished cleanup retired actor frame");
        check(frame.finish_resume(ticket), "actor cleanup did not finish");
        check(!frame.try_retire(ticket), "live pinned loan did not fence retirement");

        ores_actor_i64_heap_retire(heap);
        check(!ores_actor_i64_heap_try_destroy(heap),
              "actor heap reclaimed with shared loan across cancellation");

        // Physical cleanup is serialized by the authoritative owning actor:
        // both the metadata loan and underlying actor-owned heap loan end.
        check(frame.release(ticket, PinnedContinuationGate::Loan::shared),
              "metadata loan did not release");
        ores_actor_i64_heap_release(heap, loan);
        check(frame.try_retire(ticket) && !frame.try_retire(ticket),
              "pinned continuation double-retired");
        check(ores_actor_i64_heap_try_destroy(heap),
              "actor heap remained allocated after quiescence");
    }
}
