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
    // Exhaust on-wire u8 loan modes: only shared(0) and exclusive(1) may
    // acquire or release actor-owned frame loans. Unknown values must not
    // silently alias shared mode or release someone else's live loan.
    for (unsigned raw = 0; raw != 256; ++raw) {
        const auto mode = static_cast<PinnedContinuationGate::Loan>(raw);
        const bool admitted = raw <= 1;
        check(PinnedContinuationGate::valid_loan_mode(mode) == admitted,
              "loan-mode discriminant admission changed");
        AsyncIoTicket ticket{88, 9, static_cast<std::uint64_t>(raw + 1)};
        PinnedContinuationGate frame(ticket);
        check(frame.acquire(ticket, mode) == admitted,
              "forged loan mode acquired an actor frame");
        if (admitted) {
            const std::uint32_t expected = raw == 0 ? 1u : PinnedContinuationGate::exclusive_bit;
            check(frame.loan_state() == expected && frame.release(ticket, mode),
                  "canonical loan acquisition/release failed");
        } else {
            check(frame.loan_state() == 0, "invalid acquisition changed loan state");
            check(frame.acquire(ticket, PinnedContinuationGate::Loan::shared),
                  "shared control loan rejected");
            check(!frame.release(ticket, mode) && frame.loan_state() == 1,
                  "forged release dropped a shared loan");
            check(frame.release(ticket, PinnedContinuationGate::Loan::shared),
                  "shared control loan leaked");
            check(frame.acquire(ticket, PinnedContinuationGate::Loan::exclusive),
                  "exclusive control loan rejected");
            check(!frame.release(ticket, mode)
                  && frame.loan_state() == PinnedContinuationGate::exclusive_bit,
                  "forged release dropped an exclusive loan");
            check(frame.release(ticket, PinnedContinuationGate::Loan::exclusive),
                  "exclusive control loan leaked");
        }
        check(frame.loan_state() == 0, "loan discriminant test leaked a loan");
    }
    // A ticket is an exact actor-incarnation AND operation identity, not
    // just an operation number. Reject stale/foreign and zero-valued tokens
    // at every transition, including after cleanup and retirement.
    {
        AsyncIoTicket token{111, 222, 333};
        const AsyncIoTicket invalid[] = {
            {0, 222, 333}, {111, 0, 333}, {111, 222, 0},
            {112, 222, 333}, {111, 223, 333}, {111, 222, 334}
        };
        PinnedContinuationGate gate(token);
        AsyncIoCompletionGate io(token);
        check(gate.acquire(token, PinnedContinuationGate::Loan::shared),
              "valid loan for stale-ticket controls missing");
        for (const auto stale : invalid) {
            check(!gate.acquire(stale, PinnedContinuationGate::Loan::shared)
                    && !gate.release(stale, PinnedContinuationGate::Loan::shared)
                    && !gate.settle(stale, AsyncIoTerminal::success)
                    && !gate.broker_acknowledge(stale)
                    && !gate.try_claim_resume(stale)
                    && !gate.finish_resume(stale)
                    && !gate.try_retire(stale)
                    && !io.finish(stale, AsyncIoTerminal::success),
                  "foreign or zero ticket authorized a transition");
            check(gate.loan_state() == 1
                    && gate.terminal() == AsyncIoTerminal::pending
                    && io.terminal() == AsyncIoTerminal::pending,
                  "foreign ticket mutated pending actor frame");
        }
        check(gate.settle(token, AsyncIoTerminal::cancelled)
                && io.finish(token, AsyncIoTerminal::cancelled),
              "valid ticket failed to settle");
        for (const auto stale : invalid) {
            check(!gate.broker_acknowledge(stale)
                    && !gate.try_claim_resume(stale)
                    && !gate.finish_resume(stale)
                    && !gate.try_retire(stale),
                  "stale ticket advanced settled frame cleanup");
        }
        check(gate.broker_acknowledge(token) && gate.try_claim_resume(token),
              "valid ticket could not claim cleanup");
        for (const auto stale : invalid) {
            check(!gate.finish_resume(stale) && !gate.try_retire(stale),
                  "foreign ticket completed another actor's cleanup");
        }
        check(gate.finish_resume(token)
                && gate.release(token, PinnedContinuationGate::Loan::shared)
                && gate.try_retire(token),
              "valid cleanup could not retire frame");
        for (const auto stale : invalid) {
            check(!gate.acquire(stale, PinnedContinuationGate::Loan::shared)
                    && !gate.release(stale, PinnedContinuationGate::Loan::shared)
                    && !gate.try_retire(stale),
                  "foreign ticket mutated retired frame");
        }
        check(gate.retired()
                && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "retirement fence lost after stale-ticket attempts");
    }
    // Exhaust all on-wire u8 terminal values. Only the five declared
    // terminal outcomes may settle either the I/O admission or the pinned
    // frame; a forged value must preserve pending and prohibit broker ACK,
    // continuation resume, and physical retirement.
    for (unsigned raw = 0; raw != 256; ++raw) {
        const auto value = static_cast<AsyncIoTerminal>(raw);
        const bool valid = raw >= 1 && raw <= 5;
        check(valid_async_io_terminal(value) == valid,
              "terminal discriminant admission changed");
        AsyncIoTicket ticket{99, 7, static_cast<std::uint64_t>(raw + 1)};
        AsyncIoCompletionGate io(ticket);
        PinnedContinuationGate frame(ticket);
        check(io.finish(ticket, value) == valid,
              "I/O completion accepted an invalid terminal kind");
        check(frame.settle(ticket, value) == valid,
              "pinned actor frame accepted an invalid terminal kind");
        check(io.terminal() == (valid ? value : AsyncIoTerminal::pending),
              "invalid I/O completion corrupted terminal state");
        check(frame.terminal() == (valid ? value : AsyncIoTerminal::pending),
              "invalid frame completion corrupted terminal state");
        if (!valid) {
            check(!frame.broker_acknowledge(ticket) &&
                  !frame.try_claim_resume(ticket) &&
                  !frame.try_retire(ticket),
                  "invalid completion authorized cleanup or retirement");
        }
    }
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
    // Regression for stale loan acquisition racing frame retirement.
    // Retirement must atomically occupy the same word as loan admission.
    {
        for (std::uint64_t round = 1; round <= 256; ++round) {
            AsyncIoTicket token{77, round, round};
            PinnedContinuationGate gate(token);
            std::atomic<bool> stop{false};
            std::thread acquirer([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    if (gate.acquire(token, PinnedContinuationGate::Loan::shared)) {
                        std::this_thread::yield();
                        check(gate.release(token, PinnedContinuationGate::Loan::shared),
                              "racing shared loan release failed");
                    }
                }
            });
            check(gate.settle(token, AsyncIoTerminal::cancelled)
                    && gate.broker_acknowledge(token)
                    && gate.try_claim_resume(token)
                    && gate.finish_resume(token),
                  "retirement race did not finish cleanup");
            bool retired = false;
            for (unsigned spins = 0; spins < 1000000 && !retired; ++spins) {
                retired = gate.try_retire(token);
                if (!retired) std::this_thread::yield();
            }
            stop.store(true, std::memory_order_release);
            acquirer.join();
            check(retired && gate.retired(), "retirement could not fence loans");
            check(gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
                  "retired frame lost atomic loan fence");
            check(!gate.acquire(token, PinnedContinuationGate::Loan::shared)
                    && !gate.acquire(token, PinnedContinuationGate::Loan::exclusive)
                    && !gate.release(token, PinnedContinuationGate::Loan::shared)
                    && !gate.release(token, PinnedContinuationGate::Loan::exclusive)
                    && !gate.try_retire(token),
                  "retired frame admitted loan mutation or duplicate retirement");
        }
    }
    // Exclusive-loan release must linearize before the retirement fence.
    // Race the two CAS operations; if retirement wins while the exclusive
    // loan is live, the legitimate release will fail and this test fails.
    {
        for (std::uint64_t round = 1; round <= 256; ++round) {
            AsyncIoTicket token{78, round, round};
            PinnedContinuationGate gate(token);
            check(gate.acquire(token, PinnedContinuationGate::Loan::exclusive),
                  "exclusive race loan missing");
            check(gate.settle(token, AsyncIoTerminal::cancelled)
                    && gate.broker_acknowledge(token)
                    && gate.try_claim_resume(token)
                    && gate.finish_resume(token),
                  "exclusive race cleanup incomplete");
            check(!gate.try_retire(token),
                  "exclusive live loan permitted retirement");
            std::atomic<bool> start{false};
            bool release_ok = false;
            bool retirement_ok = false;
            std::thread releaser([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                release_ok = gate.release(token, PinnedContinuationGate::Loan::exclusive);
            });
            std::thread retirer([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (unsigned attempt = 0; attempt < 2048; ++attempt) {
                    if (gate.try_retire(token)) {
                        retirement_ok = true;
                        return;
                    }
                    std::this_thread::yield();
                }
            });
            start.store(true, std::memory_order_release);
            releaser.join();
            retirer.join();
            if (!retirement_ok) retirement_ok = gate.try_retire(token);
            check(release_ok && retirement_ok && gate.retired()
                    && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
                  "exclusive loan release/retirement race violated fence");
            check(!gate.acquire(token, PinnedContinuationGate::Loan::exclusive)
                    && !gate.release(token, PinnedContinuationGate::Loan::exclusive)
                    && !gate.try_retire(token),
                  "exclusive loan mutated retired frame");
        }
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
