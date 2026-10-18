#include "oreslang/runtime/ActorPipeRouteLedger.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>
#include <iostream>

using namespace oreslang::runtime;

static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
template <class F> static void rejected(F action, const char* reason) {
    bool blocked = false;
    try { action(); } catch (const std::exception&) { blocked = true; }
    check(blocked, reason);
}
static std::vector<PipeStage> stages() {
    return {{"Producer", PipeRole::readable, "", "Event", "source"},
            {"Worker", PipeRole::transform, "Event", "Event", "worker-a"},
            {"Worker", PipeRole::transform, "Event", "Event", "worker-b"},
            {"Sink", PipeRole::writable, "Event", "", "sink"}};
}
static std::vector<PipeConnection> edges() {
    return {{"source", "worker-a", "Event"},
            {"source", "worker-b", "Event"},
            {"worker-a", "sink", "Event"},
            {"worker-b", "sink", "Event"}};
}
int main() {
    ActorPipeRouteLedger ledger(stages(), edges(), 7, 5);
    rejected([&] { (void)ledger.ingress("worker-a"); },
             "a middle worker originated a flow");
    rejected([&] { (void)ledger.ingress("sink"); },
             "a sink originated a flow");
    const auto origin = ledger.ingress("source");
    rejected([&] { (void)ledger.forward(origin); },
             "ambiguous forward selected an arbitrary branch");
    const auto fan = ledger.forward_all(origin);
    check(fan.size() == 2 && fan[0]->receiver() == "worker-a" &&
          fan[1]->receiver() == "worker-b" &&
          fan[0]->flow_id() == fan[1]->flow_id() &&
          fan[0]->route_id() != fan[1]->route_id(),
          "fan-out lost stable branch identities");
    const auto sink_a = ledger.forward(fan[0]);
    const auto sink_b = ledger.forward(fan[1]);
    check(ledger.outstanding() == 5 && sink_a->receiver() == "sink" &&
          sink_b->receiver() == "sink", "fan-in quota / destination error");
    const auto back_a = ledger.backward(sink_a);
    const auto back_b = ledger.backward(sink_b);
    check(back_a->receiver() == "worker-a" && back_b->receiver() == "worker-b"
          && back_a->flow_id() == back_b->flow_id()
          && back_a->route_id() != back_b->route_id(),
          "fan-in reverse confused two paths");
    rejected([&] { (void)ledger.backward(sink_a); },
             "one-shot reverse route frame replay accepted");
    rejected([&] { (void)ledger.forward(back_a); },
             "reverse frame re-entered forward DAG");
    const auto reply_a = ledger.backward(back_a);
    const auto reply_b = ledger.backward(back_b);
    check(reply_a->receiver() == "source" && reply_b->receiver() == "source",
          "reverse branch missed authenticated origin");
    rejected([&] { (void)ledger.backward(reply_a); },
             "origin obtained an upstream return route");
    rejected([&] { (void)ledger.advance_generation(8); },
             "quiescence barrier ignored outstanding frames");
    ledger.retire_if_active(reply_a);
    ledger.retire_if_active(reply_a);
    ledger.retire_if_active(reply_b);
    ledger.complete(fan[0]);
    ledger.complete(fan[1]);
    ledger.complete(origin);
    check(ledger.outstanding() == 0, "reverse frames leaked credits");
    ledger.advance_generation(8);
    rejected([&] { ledger.complete(origin); }, "stale origin remained active");
    const auto next = ledger.ingress("source");
    const auto first = ledger.round_robin(next);
    check(first->receiver() == "worker-a", "RR cursor not reset at generation fence");
    const auto second = ledger.round_robin(next);
    check(second->receiver() == "worker-b",
          "round-robin cursor did not advance to the second edge");
    rejected([&] { (void)ledger.forward_to(next, "sink"); },
             "named routing bypassed authorized edges");
    ledger.complete(first);
    ledger.complete(second);
    ledger.complete(next);
    // Normal quiescent epoch transition must reset RR without erasing
    // constructor-preallocated source selector nodes.
    ledger.advance_generation(9);
    const auto epoch_root = ledger.ingress("source");
    const auto epoch_first = ledger.round_robin(epoch_root);
    check(epoch_first->receiver() == "worker-a",
          "second generation failed to reset the round-robin selector");
    ledger.complete(epoch_first);
    ledger.complete(epoch_root);
    ledger.close();
    rejected([&] { (void)ledger.ingress("source"); },
             "closed generation accepted new DATA");

    // A caller dropping the last external Handle must NOT free a still
    // active Frame: otherwise a later allocation may reuse its address and
    // accidentally pass the raw-pointer identity check (ABA). The ledger
    // owns the frame until explicit completion, abort or STOP.
    {
        ActorPipeRouteLedger retained(stages(), edges(), 11, 2);
        auto root = retained.ingress("source");
        std::weak_ptr<const ActorPipeRouteLedger::Frame> weak;
        {
            auto orphan = retained.round_robin(root);
            weak = orphan;
            check(retained.outstanding() == 2, "lost active child");
        }
        check(!weak.expired(), "active route lost strong ownership");
        rejected([&] { (void)retained.round_robin(root); },
                 "dropped handle bypassed the correlation frame budget");
        rejected([&] { retained.advance_generation(12); },
                 "dropped but active frame bypassed quiescence");
        check(retained.abort_and_advance(12) == 2,
              "abort did not retire orphaned active handles");
        check(weak.expired(), "aborted route frame retained forever");
        rejected([&] { retained.complete(root); },
                 "aborted root remained active");
        // Forced epoch rollover must also retain/reset selector state.
        auto restarted = retained.ingress("source");
        const auto restarted_first = retained.round_robin(restarted);
        check(restarted_first->receiver() == "worker-a",
              "aborted generation failed to reset first route");
        retained.complete(restarted_first);
        const auto restarted_second = retained.round_robin(restarted);
        check(restarted_second->receiver() == "worker-b",
              "aborted generation lost route order");
        retained.complete(restarted_second);
        retained.complete(restarted);
        retained.close();
    }

    // A routed attempt consumes its RR turn even if child admission fails.
    // Exhaust the frame budget after selecting A, attempt and fail B, then
    // release A: the next successful turn must be A, not B.
    {
        ActorPipeRouteLedger limited(stages(), edges(), 16, 2);
        const auto root = limited.ingress("source");
        const auto selected_a = limited.round_robin(root);
        check(selected_a->receiver() == "worker-a",
              "budget test first selection was not A");
        rejected([&] { (void)limited.round_robin(root); },
                 "RR exceeded frame budget without rejection");
        check(limited.outstanding() == 2,
              "failed RR admission corrupted frame credits");
        limited.complete(selected_a);
        const auto after_failed_b = limited.round_robin(root);
        check(after_failed_b->receiver() == "worker-a",
              "failed RR attempt did not consume B's turn");
        limited.complete(after_failed_b);
        limited.complete(root);
        limited.advance_generation(17);
        const auto next_root = limited.ingress("source");
        const auto next_a = limited.round_robin(next_root);
        check(next_a->receiver() == "worker-a",
              "quota failure left stale RR state after generation change");
        limited.complete(next_a);
        limited.complete(next_root);
    }

    // A route frame is an exclusive, one-shot publication candidate.
    // A rejected enqueue may release only the lease held by that sender,
    // while an accepted enqueue irrevocably seals its frame identity.
    {
        ActorPipeRouteLedger publication(stages(), edges(), 30, 4);
        auto origin = publication.ingress("source");
        {
            auto first = publication.claim_publication(origin);
            rejected([&] { (void)publication.claim_publication(origin); },
                     "second native publisher stole a live route claim");
            rejected([&] { (void)publication.abort_and_advance(31); },
                     "epoch abort discarded claimed native route frame");
            check(!origin->published() && publication.outstanding() == 1,
                  "rejected abort corrupted live native frame");
            // First lease's destructor, NOT the rejected contender,
            // is solely responsible for freeing the claim.
        }
        {
            auto retry = publication.claim_publication(origin);
            retry.commit(); // successful mailbox tryWrite boundary
            retry.abort();  // cannot undo already accepted data
        }
        check(origin->published(), "accepted native publication marker was lost");
        rejected([&] { (void)publication.claim_publication(origin); },
                 "published native route frame was replayed");
        publication.complete(origin);
        check(origin->published(),
              "native route retirement incorrectly reset publication status");
        publication.advance_generation(31);
        const auto replacement = publication.ingress("source");
        auto in_flight = publication.claim_publication(replacement);
        rejected([&] { (void)publication.abort_and_advance(32); },
                 "new generation abandoned a claimed native mailbox route");
        in_flight.abort();
        check(publication.abort_and_advance(32) == 1,
              "native prepublication rejection failed to permit epoch abort");
        rejected([&] { (void)publication.claim_publication(replacement); },
                 "native old-generation frame survived epoch abort");
        publication.close();
    }

    // Eight contending host publishers must produce exactly one claim.
    // Keep the successful lease alive until all losing contenders exit,
    // otherwise later sequential reservations would be legal retries.
    {
        ActorPipeRouteLedger contested(stages(), edges(), 31, 4);
        auto origin = contested.ingress("source");
        std::atomic<int> ready{0};
        std::atomic<int> attempted{0};
        std::atomic<int> won{0};
        std::atomic<int> denied{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> contenders;
        for (int t = 0; t < 8; ++t) {
            contenders.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                try {
                    auto claim = contested.claim_publication(origin);
                    won.fetch_add(1, std::memory_order_acq_rel);
                    attempted.fetch_add(1, std::memory_order_acq_rel);
                    while (attempted.load(std::memory_order_acquire) < 8)
                        std::this_thread::yield();
                    claim.commit();
                } catch (const std::logic_error&) {
                    denied.fetch_add(1, std::memory_order_acq_rel);
                    attempted.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != 8)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& contender : contenders) contender.join();
        check(won == 1 && denied == 7 && origin->published(),
              "native competing publishers admitted duplicate correlation DATA");
        contested.complete(origin);
        check(contested.outstanding() == 0, "native claim leaked route credits");
    }

    // Claim leases only refund the frame they actually acquired; a
    // failed contender never receives the authority to abort the winner.
    {
        ActorPipeRouteLedger rejected_send(stages(), edges(), 32, 3);
        auto original = rejected_send.ingress("source");
        auto winner = rejected_send.claim_publication(original);
        rejected([&] { (void)rejected_send.claim_publication(original); },
                 "losing sender received an abort-capable publication lease");
        rejected([&] { (void)rejected_send.claim_publication(original); },
                 "losing sender released winner's claim during cleanup");
        winner.abort(); // actual owner receives definite rejection
        auto retry = rejected_send.claim_publication(original);
        retry.commit();
        check(original->published(), "retry failed to commit accepted DATA");
        rejected_send.complete(original);
    }

    // Selection/retirement of 2000 independent child frames is synchronized.
    ActorPipeRouteLedger parallel(stages(), edges(), 9, 2001);
    const auto parent = parallel.ingress("source");
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int j = 0; j < 500; ++j) {
                const auto child = parallel.round_robin(parent);
                parallel.complete(child);
                ++completed;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(completed == 2000 && parallel.outstanding() == 1,
          "concurrent route IDs or frame retirement raced");
    parallel.complete(parent);
    std::cout << "ORES_NATIVE_ACTOR_ROUTE_LEDGER_OK branches=2000" << std::endl;
}
