#include "oreslang/runtime/PinnedContinuationGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
static void check(bool success, const char* description) {
    if (!success) throw std::runtime_error(description);
}
int main() {
    for (std::uint64_t i = 1; i <= 1200; ++i) {
        AsyncIoTicket token{21, i + 40, i};
        PinnedContinuationGate gate(token);
        check(!gate.acquire({21, i + 40, i + 1}, PinnedContinuationGate::Loan::shared),
              "stale generation/operation acquired loan");
        check(!gate.broker_acknowledge(token) && !gate.try_claim_resume(token)
              && !gate.finish_resume(token) && !gate.try_retire(token),
              "unsettled frame retired or resumed");
        check(gate.acquire(token, PinnedContinuationGate::Loan::shared), "first shared loan");
        check(gate.acquire(token, PinnedContinuationGate::Loan::shared), "second shared loan");
        check(!gate.acquire(token, PinnedContinuationGate::Loan::exclusive), "overlapping exclusive alias");
        std::atomic<unsigned> wins{0};
        std::thread a([&]{ if (gate.settle(token, AsyncIoTerminal::success)) ++wins; });
        std::thread b([&]{ if (gate.settle(token, AsyncIoTerminal::cancelled)) ++wins; });
        std::thread c([&]{ if (gate.settle(token, AsyncIoTerminal::timed_out)) ++wins; });
        a.join(); b.join(); c.join();
        check(wins == 1 && gate.terminal() != AsyncIoTerminal::pending,
              "completion/cancellation/timeout did not arbitrate exactly once");
        check(!gate.acquire(token, PinnedContinuationGate::Loan::shared),
              "loan acquired after settlement");
        check(!gate.try_claim_resume(token) && !gate.try_retire(token),
              "unacknowledged broker enabled resume/retire");
        check(gate.broker_acknowledge(token), "broker ACK missing");
        check(!gate.broker_acknowledge(token), "duplicate broker ACK");
        check(gate.try_claim_resume(token) && !gate.try_claim_resume(token),
              "resumption not exactly once");
        check(!gate.try_retire(token), "claimed but unfinished resumption freed frame");
        check(gate.finish_resume(token) && !gate.finish_resume(token),
              "resume completion duplicated");
        check(!gate.try_retire(token), "frame retired with live loan");
        check(gate.release(token, PinnedContinuationGate::Loan::shared), "release first");
        check(!gate.try_retire(token), "frame retired with second loan");
        check(gate.release(token, PinnedContinuationGate::Loan::shared), "release second");
        check(!gate.release(token, PinnedContinuationGate::Loan::shared), "loan double-release");
        check(gate.try_retire(token) && !gate.try_retire(token), "double retirement");
        check(!gate.settle(token, AsyncIoTerminal::failure), "late completion overwrote winner");
    }
    {
        AsyncIoTicket token{4, 7, 1};
        PinnedContinuationGate gate(token);
        check(gate.acquire(token, PinnedContinuationGate::Loan::exclusive), "exclusive loan absent");
        check(!gate.acquire(token, PinnedContinuationGate::Loan::shared)
            && !gate.release(token, PinnedContinuationGate::Loan::shared),
            "exclusive loan aliased or wrong-mode released");
        check(gate.settle(token, AsyncIoTerminal::closed) &&
              gate.broker_acknowledge(token) && gate.try_claim_resume(token),
              "closed operation did not clean up");
        check(!gate.try_retire(token), "frame retired before cleanup finished");
        check(gate.finish_resume(token) && !gate.try_retire(token),
              "live exclusive borrow retired");
        check(gate.release(token, PinnedContinuationGate::Loan::exclusive)
            && gate.try_retire(token), "exclusive cleanup leaked");
    }
    // A synchronous producer callback may settle the operation before the
    // registering host call unwinds. The metadata gate still cannot admit
    // a guest continuation without explicit broker quiescence and a distinct
    // scheduler-owned turn. This is a host-only no-Zalgo prerequisite, NOT a
    // compiler-generated await/select conformance test.
    {
        AsyncIoTicket token{31, 11, 7};
        PinnedContinuationGate gate(token);
        std::atomic<bool> producer_was_inside_callback{false};
        std::thread producer([&] {
            producer_was_inside_callback.store(true, std::memory_order_release);
            check(gate.settle(token, AsyncIoTerminal::success),
                  "synchronous foreign callback failed to settle");
            check(!gate.try_claim_resume(token),
                  "foreign completion admitted guest resume before broker ACK");
            producer_was_inside_callback.store(false, std::memory_order_release);
        });
        producer.join();
        check(!producer_was_inside_callback.load(std::memory_order_acquire),
              "foreign callback still executing");
        check(!gate.try_claim_resume(token),
              "registration-time completion was treated as an inline guest resume");
        check(gate.broker_acknowledge(token), "registration quiescence ACK missing");
        check(gate.try_claim_resume(token), "scheduler could not claim deferred guest work");
        check(!gate.try_claim_resume(token), "same completion resumed twice");
        check(gate.finish_resume(token) && gate.try_retire(token),
              "guest cleanup or frame retirement missing");
    }
    {
        AsyncIoTicket token{4, 8, 2};
        PinnedContinuationGate gate(token);
        for (unsigned n=0; n<PinnedContinuationGate::max_shared_loans; ++n)
            check(gate.acquire(token, PinnedContinuationGate::Loan::shared), "bounded loan overflow");
        check(!gate.acquire(token, PinnedContinuationGate::Loan::shared), "loan registry unbounded");
        for (unsigned n=0; n<PinnedContinuationGate::max_shared_loans; ++n)
            check(gate.release(token, PinnedContinuationGate::Loan::shared), "bounded loan leak");
        check(gate.settle(token, AsyncIoTerminal::success) &&
              gate.broker_acknowledge(token) && gate.try_claim_resume(token) &&
              gate.finish_resume(token) && gate.try_retire(token),
              "bounded frame leaked after completion");
    }
}
