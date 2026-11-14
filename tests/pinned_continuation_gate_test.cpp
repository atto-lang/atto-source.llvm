#include "oreslang/runtime/PinnedContinuationGate.hpp"
#include <atomic>
#include <cstdint>
#include <new>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

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

    // Host-only borrower receipts: operation identity is not release authority.
    {
        static_assert(!std::is_default_constructible_v<BorrowerPinnedContinuationGate::Receipt>,
                      "receipts must be issued by the gate");
        AsyncIoTicket ticket{121, 5, 9};
        BorrowerPinnedContinuationGate gate(ticket);
        auto a = gate.acquire(ticket, 101, BorrowerPinnedContinuationGate::Loan::shared);
        auto b = gate.acquire(ticket, 202, BorrowerPinnedContinuationGate::Loan::shared);
        check(a.has_value() && b.has_value() && gate.loan_state() == 2,
              "distinct borrower receipts not admitted");
        check(!gate.release(ticket, 202, *a) && gate.loan_state() == 2,
              "wrong borrower consumed another borrower's receipt");
        check(!gate.release({121, 6, 9}, 101, *a) && gate.loan_state() == 2,
              "stale generation consumed receipt");
        check(!gate.release({121, 5, 10}, 101, *a) && gate.loan_state() == 2,
              "stale operation consumed receipt");
        check(gate.settle(ticket, AsyncIoTerminal::cancelled)
                  && gate.broker_acknowledge(ticket)
                  && gate.try_claim_resume(ticket)
                  && gate.finish_resume(ticket)
                  && !gate.try_retire(ticket),
              "live receipts permitted retirement");
        check(gate.release(ticket, 101, *a) && !gate.release(ticket, 101, *a),
              "duplicate receipt release was not rejected");
        check(!gate.try_retire(ticket) && gate.release(ticket, 202, *b)
                  && gate.try_retire(ticket) && !gate.try_retire(ticket),
              "borrower receipt retirement fence failed");
        check(!gate.acquire(ticket, 303, BorrowerPinnedContinuationGate::Loan::shared)
                  && !gate.release(ticket, 202, *b),
              "retired frame accepted receipt mutation");
    }
    {
        AsyncIoTicket ticket{122, 6, 10};
        BorrowerPinnedContinuationGate gate(ticket);
        auto x = gate.acquire(ticket, 401, BorrowerPinnedContinuationGate::Loan::exclusive);
        check(x.has_value() && !gate.acquire(ticket, 402, BorrowerPinnedContinuationGate::Loan::shared),
              "exclusive borrower admitted conflicting loan");
        check(!gate.release(ticket, 402, *x) && gate.loan_state() == PinnedContinuationGate::exclusive_bit,
              "wrong exclusive borrower released loan");
        check(gate.settle(ticket, AsyncIoTerminal::success)
                  && gate.broker_acknowledge(ticket)
                  && gate.try_claim_resume(ticket)
                  && gate.finish_resume(ticket)
                  && !gate.try_retire(ticket)
                  && gate.release(ticket, 401, *x)
                  && gate.try_retire(ticket),
              "exclusive receipt cleanup failed");
    }
    {
        AsyncIoTicket ticket{123, 7, 11};
        BorrowerPinnedContinuationGate gate(ticket);
        std::vector<BorrowerPinnedContinuationGate::Receipt> receipts;
        for (std::size_t i = 0; i < BorrowerPinnedContinuationGate::max_receipts; ++i) {
            auto r = gate.acquire(ticket, i + 1, BorrowerPinnedContinuationGate::Loan::shared);
            check(r.has_value(), "receipt capacity admitted fewer than bound");
            receipts.push_back(*r);
        }
        check(!gate.acquire(ticket, 999, BorrowerPinnedContinuationGate::Loan::shared)
                  && gate.loan_state() == BorrowerPinnedContinuationGate::max_receipts,
              "receipt registry overflow did not fail closed");
        for (std::size_t i = 0; i < receipts.size(); ++i)
            check(gate.release(ticket, i + 1, receipts[i]), "receipt registry leaked loan");
        check(gate.settle(ticket, AsyncIoTerminal::closed)
                  && gate.broker_acknowledge(ticket)
                  && gate.try_claim_resume(ticket)
                  && gate.finish_resume(ticket)
                  && gate.try_retire(ticket), "bounded registry failed to retire");
    }
    // A destroyed gate may be reconstructed at the EXACT same address with
    // the same actor ticket, borrower, slot and serial. A receipt from the
    // previous incarnation must not authorize release of the new loan.
    // This is an in-process host identity regression, not cross-DSO/FFI proof.
    for (std::uint64_t round = 1; round <= 64; ++round) {
        AsyncIoTicket ticket{130, round, round};
        alignas(BorrowerPinnedContinuationGate)
            unsigned char storage[sizeof(BorrowerPinnedContinuationGate)];
        auto* first = new (storage) BorrowerPinnedContinuationGate(ticket);
        auto stale = first->acquire(ticket, 77,
            BorrowerPinnedContinuationGate::Loan::shared);
        check(stale.has_value() && first->loan_state() == 1,
              "reused-storage first incarnation did not acquire");
        check(first->settle(ticket, AsyncIoTerminal::cancelled)
                && first->broker_acknowledge(ticket)
                && first->try_claim_resume(ticket)
                && first->finish_resume(ticket)
                && first->release(ticket, 77, *stale)
                && first->try_retire(ticket),
              "reused-storage first incarnation did not retire cleanly");
        first->~BorrowerPinnedContinuationGate();
        auto* second = new (storage) BorrowerPinnedContinuationGate(ticket);
        auto fresh = second->acquire(ticket, 77,
            BorrowerPinnedContinuationGate::Loan::shared);
        check(fresh.has_value() && second->loan_state() == 1,
              "reused-storage second incarnation did not acquire");
        check(!second->release(ticket, 77, *stale)
                && second->loan_state() == 1,
              "stale receipt crossed gate lifetime at reused address");
        check(second->settle(ticket, AsyncIoTerminal::success)
                && second->broker_acknowledge(ticket)
                && second->try_claim_resume(ticket)
                && second->finish_resume(ticket)
                && !second->try_retire(ticket)
                && second->release(ticket, 77, *fresh)
                && second->try_retire(ticket)
                && second->loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "reused-storage second incarnation failed safe retirement");
        second->~BorrowerPinnedContinuationGate();
    }
    // Two gate instances may have identical tickets and per-gate serials.
    // A copied receipt from one incarnation must never release the other.
    {
        AsyncIoTicket ticket{125, 8, 12};
        BorrowerPinnedContinuationGate first(ticket);
        BorrowerPinnedContinuationGate second(ticket);
        auto a = first.acquire(ticket, 77, BorrowerPinnedContinuationGate::Loan::shared);
        auto b = second.acquire(ticket, 77, BorrowerPinnedContinuationGate::Loan::shared);
        check(a.has_value() && b.has_value(), "same-ticket gate setup failed");
        check(!first.release(ticket, 77, *b)
                  && !second.release(ticket, 77, *a)
                  && first.loan_state() == 1 && second.loan_state() == 1,
              "cross-incarnation receipt released another gate's loan");
        check(first.release(ticket, 77, *a) && second.release(ticket, 77, *b),
              "own-incarnation receipts failed to release");
        auto reused = first.acquire(ticket, 77, BorrowerPinnedContinuationGate::Loan::shared);
        check(reused.has_value() && !first.release(ticket, 77, *a)
                  && first.loan_state() == 1,
              "stale receipt consumed reused slot");
        check(first.release(ticket, 77, *reused), "reused slot could not release");
    }
    // Cancellation/completion racing admission must either reject a new
    // receipt or leave a real, releasable loan that blocks retirement.
    // Invalid borrower and loan modes must never allocate a registry slot.
    for (std::uint64_t round = 1; round <= 256; ++round) {
        AsyncIoTicket ticket{126, round, round};
        BorrowerPinnedContinuationGate gate(ticket);
        const auto mode = round % 2 ? BorrowerPinnedContinuationGate::Loan::shared
                                    : BorrowerPinnedContinuationGate::Loan::exclusive;
        const auto invalid_mode = static_cast<BorrowerPinnedContinuationGate::Loan>(255);
        check(!gate.acquire(ticket, 0, mode)
                  && !gate.acquire(ticket, round, invalid_mode)
                  && gate.loan_state() == 0,
              "invalid borrower or loan mode mutated receipt registry");
        std::atomic<bool> start{false};
        std::optional<BorrowerPinnedContinuationGate::Receipt> acquired;
        std::thread acquirer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            acquired = gate.acquire(ticket, round, mode);
        });
        start.store(true, std::memory_order_release);
        const auto outcome = round % 2 ? AsyncIoTerminal::cancelled
                                        : AsyncIoTerminal::success;
        check(gate.settle(ticket, outcome),
              "racing terminal settlement lost its one winner");
        acquirer.join();
        check(gate.terminal() == outcome
                  && !gate.acquire(ticket, round + 1, mode),
              "settled gate admitted a new borrower");
        check(gate.broker_acknowledge(ticket)
                  && gate.try_claim_resume(ticket)
                  && gate.finish_resume(ticket),
              "racing admission cleanup did not finish");
        if (acquired) {
            check(!gate.try_retire(ticket),
                  "racing admission receipt did not pin frame");
            check(!gate.release(ticket, round + 1, *acquired),
                  "foreign borrower consumed racing receipt");
            check(gate.release(ticket, round, *acquired)
                      && !gate.release(ticket, round, *acquired),
                  "racing receipt was leaked or double-released");
        }
        check(gate.try_retire(ticket) && !gate.try_retire(ticket)
                  && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "racing admission allowed premature or duplicate retirement");
    }
    // Two threads holding copies of ONE receipt must not both consume it.
    // The registry removal and loan decrement must linearize exactly once,
    // even when both callers present the same valid borrower identity.
    for (std::uint64_t round = 1; round <= 128; ++round) {
        AsyncIoTicket ticket{129, round, round};
        BorrowerPinnedContinuationGate gate(ticket);
        auto receipt = gate.acquire(ticket, 77,
            BorrowerPinnedContinuationGate::Loan::shared);
        check(receipt.has_value() && gate.loan_state() == 1,
              "duplicate-release race setup failed");
        std::atomic<bool> start{false};
        std::atomic<unsigned> wins{0};
        auto release_same = [&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (gate.release(ticket, 77, *receipt))
                wins.fetch_add(1, std::memory_order_relaxed);
        };
        std::thread a(release_same);
        std::thread b(release_same);
        start.store(true, std::memory_order_release);
        a.join();
        b.join();
        check(wins.load(std::memory_order_relaxed) == 1
                && gate.loan_state() == 0
                && !gate.release(ticket, 77, *receipt),
              "concurrent duplicate receipt released twice or leaked loan");
        check(gate.settle(ticket, AsyncIoTerminal::cancelled)
                && gate.broker_acknowledge(ticket)
                && gate.try_claim_resume(ticket)
                && gate.finish_resume(ticket)
                && gate.try_retire(ticket)
                && !gate.try_retire(ticket)
                && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "duplicate-release race violated retirement fence");
    }
    // Multiple borrowers drain receipts concurrently while a scheduler
    // probes retirement. A final sentinel receipt must prevent reclamation,
    // regardless of the mutex contention or the other releases' ordering.
    {
        AsyncIoTicket ticket{128, 17, 21};
        BorrowerPinnedContinuationGate gate(ticket);
        std::vector<BorrowerPinnedContinuationGate::Receipt> receipts;
        for (std::uint64_t borrower = 1;
             borrower <= BorrowerPinnedContinuationGate::max_receipts; ++borrower) {
            auto receipt = gate.acquire(ticket, borrower,
                BorrowerPinnedContinuationGate::Loan::shared);
            check(receipt.has_value(), "concurrent receipt drain setup failed");
            receipts.push_back(*receipt);
        }
        check(gate.settle(ticket, AsyncIoTerminal::cancelled)
                && gate.broker_acknowledge(ticket)
                && gate.try_claim_resume(ticket)
                && gate.finish_resume(ticket),
              "concurrent receipt drain cleanup incomplete");
        std::atomic<bool> start{false};
        std::atomic<unsigned> released{0};
        auto drain = [&](std::size_t first, std::size_t last) {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::size_t i = first; i < last; ++i)
                if (gate.release(ticket, static_cast<std::uint64_t>(i + 1), receipts[i]))
                    released.fetch_add(1, std::memory_order_relaxed);
        };
        std::thread a(drain, 0, 21);
        std::thread b(drain, 21, 42);
        std::thread c(drain, 42, 63);
        start.store(true, std::memory_order_release);
        for (unsigned probe = 0; probe < 1024; ++probe)
            check(!gate.try_retire(ticket),
                  "retirement bypassed live sentinel receipt under contention");
        a.join(); b.join(); c.join();
        check(released.load(std::memory_order_relaxed) == 63
                && gate.loan_state() == 1 && !gate.try_retire(ticket),
              "concurrent borrower drain lost a receipt or retired prematurely");
        check(gate.release(ticket, 64, receipts[63])
                && gate.try_retire(ticket) && !gate.try_retire(ticket)
                && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "concurrent borrower drain failed permanent retirement fence");
    }
    // Receipt release and retirement serialize through the adapter mutex;
    // retirement probes fail closed rather than waiting on contention.
    for (std::uint64_t round = 1; round <= 128; ++round) {
        AsyncIoTicket ticket{124, round, round};
        BorrowerPinnedContinuationGate gate(ticket);
        auto receipt = gate.acquire(ticket, 500, BorrowerPinnedContinuationGate::Loan::shared);
        check(receipt.has_value(), "racing receipt missing");
        check(gate.settle(ticket, AsyncIoTerminal::cancelled)
                  && gate.broker_acknowledge(ticket)
                  && gate.try_claim_resume(ticket)
                  && gate.finish_resume(ticket), "racing cleanup incomplete");
        std::atomic<bool> start{false};
        bool released = false;
        std::thread releaser([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            released = gate.release(ticket, 500, *receipt);
        });
        start.store(true, std::memory_order_release);
        bool retired = false;
        for (unsigned attempt = 0; attempt < 100000 && !retired; ++attempt) {
            retired = gate.try_retire(ticket);
            if (!retired) std::this_thread::yield();
        }
        releaser.join();
        if (!retired) retired = gate.try_retire(ticket);
        check(released && retired && gate.retired()
                  && gate.loan_state() == PinnedContinuationGate::retirement_fence_bit,
              "borrower receipt release/retirement race violated fence");
    }

    // Concurrent gate construction must issue unique process-local
    // incarnation identities. Identical tickets, borrowers, and first-slot
    // serials must not make receipts interchangeable across gate instances.
    // This does not prove uniqueness across shared libraries or processes.
    {
        constexpr std::size_t instances = 32;
        const AsyncIoTicket ticket{131, 9, 23};
        using Gate = BorrowerPinnedContinuationGate;
        std::vector<std::unique_ptr<Gate>> gates(instances);
        std::vector<std::optional<Gate::Receipt>> receipts(instances);
        std::vector<std::thread> constructors;
        std::atomic<bool> start{false};
        for (std::size_t i = 0; i < instances; ++i) {
            constructors.emplace_back([&, i] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                gates[i] = std::make_unique<Gate>(ticket);
                receipts[i] = gates[i]->acquire(ticket, 77, Gate::Loan::shared);
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& worker : constructors) worker.join();
        for (std::size_t i = 0; i < instances; ++i) {
            check(receipts[i].has_value() && gates[i]->loan_state() == 1,
                  "concurrent gate construction failed to issue receipt");
            for (std::size_t j = 0; j < instances; ++j) {
                if (i == j) continue;
                check(!gates[i]->release(ticket, 77, *receipts[j])
                          && gates[i]->loan_state() == 1,
                      "concurrent gate incarnation reused another receipt identity");
            }
            check(gates[i]->release(ticket, 77, *receipts[i])
                      && !gates[i]->release(ticket, 77, *receipts[i])
                      && gates[i]->settle(ticket, AsyncIoTerminal::cancelled)
                      && gates[i]->broker_acknowledge(ticket)
                      && gates[i]->try_claim_resume(ticket)
                      && gates[i]->finish_resume(ticket)
                      && gates[i]->try_retire(ticket)
                      && gates[i]->loan_state() == PinnedContinuationGate::retirement_fence_bit,
                  "concurrent gate incarnation failed single-use cleanup");
        }
    }

}
