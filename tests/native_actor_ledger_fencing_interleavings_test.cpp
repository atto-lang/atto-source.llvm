#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Ledger = oreslang::runtime::ActorExecutionLedger<2>;
using Status = Ledger::Status;
using Kind = Ledger::Kind;
using Dispatch = Ledger::Dispatch;

static void check(bool value, const char* what, std::size_t round) {
    if (!value) {
        std::fprintf(stderr, "ACTOR_LEDGER_FENCE_FAIL round=%zu: %s\n", round, what);
        std::abort();
    }
}

static void early_completion_never_parks(std::size_t round) {
    Ledger ledger(round + 1, 7);
    Ledger::Token a{}, b{};
    Ledger::Selection chosen{};
    check(ledger.admit(Kind::Mail, a) == Status::Ok, "admit A", round);
    check(ledger.admit(Kind::Timer, b) == Status::Ok, "admit B", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.token.sequence == a.sequence, "FIFO A", round);
    check(ledger.prepare_await(a) == Status::Ok, "arm A", round);
    check(ledger.wake(a) == Status::Ok, "early WAKE", round);
    check(ledger.suspend(a) == Status::AlreadyReady,
          "early WAKE must never park", round);
    auto state = ledger.snapshot();
    check(state.executing && !state.suspended && !state.runnable,
          "actor lease retained after early WAKE", round);
    check(ledger.claim_under_lease(chosen) == Status::Busy,
          "B cannot reenter before physical exit", round);
    check(ledger.finish_after_exit(a) == Status::Ok, "A exits", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.token.sequence == b.sequence, "FIFO B after A exit", round);
    check(ledger.finish_after_exit(b) == Status::Ok, "B exits", round);
    check(ledger.stop_and_request_cleanup() == Status::Ok &&
          ledger.physically_retired(), "fully retired", round);
}

static void stopped_suspension_requires_authenticated_cleanup(std::size_t round) {
    Ledger ledger(round + 1, 8);
    Ledger::Token a{}, b{};
    Ledger::Selection chosen{};
    check(ledger.admit(Kind::Mail, a) == Status::Ok, "admit A", round);
    check(ledger.admit(Kind::Deferred, b) == Status::Ok, "admit B", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok, "claim A", round);
    check(ledger.prepare_await(a) == Status::Ok, "arm A", round);
    check(ledger.suspend(a) == Status::Ok, "park A", round);
    check(ledger.stop_and_request_cleanup() == Status::Ok, "STOP", round);
    check(!ledger.physically_retired(), "suspended frame survives STOP", round);
    check(ledger.admit(Kind::Mail, b) == Status::Closed,
          "no admission following STOP", round);
    check(ledger.wake(a) == Status::AlreadyCancelled,
          "late WAKE does not restart guest", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.dispatch == Dispatch::CancelCleanup &&
          chosen.token.sequence == a.sequence,
          "STOP resumes only A cleanup", round);
    check(!ledger.physically_retired(), "not retired during cleanup", round);
    check(ledger.finish_after_exit(a) == Status::Ok,
          "cleanup completed after guest exit", round);
    check(ledger.physically_retired(), "quiesced after A cleanup", round);
    check(ledger.wake(a) == Status::Stale, "old WAKE fenced", round);
    check(ledger.stop_and_request_cleanup() == Status::Closed,
          "STOP exactly once", round);
}

static void ready_cancellation_preempts_later_user_mail(std::size_t round) {
    Ledger ledger(round + 1, 9);
    Ledger::Token a{}, b{};
    Ledger::Selection chosen{};
    check(ledger.admit(Kind::Mail, a) == Status::Ok, "admit A", round);
    check(ledger.admit(Kind::Timer, b) == Status::Ok, "admit B", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok, "claim A", round);
    check(ledger.prepare_await(a) == Status::Ok, "arm A", round);
    check(ledger.suspend(a) == Status::Ok, "suspend A", round);
    check(ledger.wake(a) == Status::Ok, "wake A", round);
    check(ledger.cancel(a) == Status::Ok, "cancel ready A", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.token.sequence == a.sequence &&
          chosen.dispatch == Dispatch::CancelCleanup,
          "A cleanup wins over queued B", round);
    check(ledger.claim_under_lease(chosen) == Status::Busy,
          "no B during cleanup", round);
    check(ledger.finish_after_exit(a) == Status::Ok, "cleanup exit", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.token.sequence == b.sequence &&
          chosen.dispatch == Dispatch::Normal,
          "B runs only after cleanup", round);
    check(ledger.finish_after_exit(b) == Status::Ok, "B exit", round);
    check(ledger.stop_and_request_cleanup() == Status::Ok &&
          ledger.physically_retired(), "retired", round);
}

static void slot_reuse_rejects_stale_token(std::size_t round) {
    Ledger ledger(round + 1, 10);
    Ledger::Token first{}, second{};
    Ledger::Selection chosen{};
    check(ledger.admit(Kind::Mail, first) == Status::Ok, "admit first", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok, "claim first", round);
    check(ledger.finish_after_exit(first) == Status::Ok, "first exit", round);
    check(ledger.admit(Kind::Mail, second) == Status::Ok,
          "reuse slot", round);
    check(first.slot == second.slot &&
          first.slot_generation != second.slot_generation,
          "generational slot fencing", round);
    check(ledger.wake(first) == Status::Stale, "stale wake", round);
    check(ledger.cancel(first) == Status::Stale, "stale cancel", round);
    auto forged = second;
    ++forged.actor;
    check(ledger.cancel(forged) == Status::Stale, "foreign actor", round);
    forged = second;
    ++forged.generation;
    check(ledger.cancel(forged) == Status::Stale,
          "foreign incarnation", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok &&
          chosen.token.sequence == second.sequence, "new token executes", round);
    check(ledger.finish_after_exit(second) == Status::Ok, "new exit", round);
    check(ledger.stop_and_request_cleanup() == Status::Ok &&
          ledger.physically_retired(), "retired", round);
}

static void invalid_kind_cannot_consume_admission_sequence(std::size_t round) {
    Ledger ledger(round + 1, 11);
    Ledger::Token token{};
    Ledger::Selection chosen{};
    check(ledger.admit(static_cast<Kind>(255), token) == Status::Invalid,
          "unknown event denied", round);
    check(ledger.snapshot().live == 0, "invalid event spends no credit", round);
    check(ledger.admit(Kind::Mail, token) == Status::Ok &&
          token.sequence == 1, "sequence unchanged", round);
    check(ledger.claim_under_lease(chosen) == Status::Ok, "claim", round);
    check(ledger.stop_and_request_cleanup() == Status::Ok,
          "STOP while active", round);
    check(!ledger.physically_retired(), "active turn remains pinned", round);
    check(ledger.prepare_await(token) == Status::Cancelled,
          "no new await after STOP", round);
    check(ledger.finish_after_exit(token) == Status::Ok,
          "physical active exit", round);
    check(ledger.physically_retired(), "retired after physical exit", round);
}

int main() {
    constexpr std::size_t rounds = 2500;
    for (std::size_t r=0; r<rounds; ++r) {
        early_completion_never_parks(r);
        stopped_suspension_requires_authenticated_cleanup(r);
        ready_cancellation_preempts_later_user_mail(r);
        slot_reuse_rejects_stale_token(r);
        invalid_kind_cannot_consume_admission_sequence(r);
    }
    std::printf("ACTOR_LEDGER_FENCE_INTERLEAVINGS_PASS rounds=%zu cases=%zu\n",
                rounds, rounds*5);
}
