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
    rejected([&] { (void)ledger.forward_to(next, "sink"); },
             "named routing bypassed authorized edges");
    ledger.complete(first);
    ledger.complete(next);
    ledger.close();
    rejected([&] { (void)ledger.ingress("source"); },
             "closed generation accepted new DATA");

    // A true direct source reply bypasses intermediate actor handlers but
    // keeps the full original per-branch correlation path and route ID.
    {
        ActorPipeRouteLedger direct(stages(), edges(), 19, 5);
        const auto root = direct.ingress("source");
        rejected([&] { (void)direct.reply(root); },
                 "ingress manufactured a direct reply to itself");
        const auto fork = direct.forward_all(root);
        const auto sink_left = direct.forward(fork[0]);
        const auto sink_right = direct.forward(fork[1]);
        const auto answer_left = direct.reply(sink_left);
        const auto answer_right = direct.reply(sink_right);
        check(answer_left->receiver() == "source" &&
              answer_right->receiver() == "source",
              "direct reply went to a guessed or intermediate actor");
        check(answer_left->flow_id() == answer_right->flow_id() &&
              answer_left->route_id() != answer_right->route_id() &&
              answer_left->returning() && answer_right->returning(),
              "direct fan-in replies collapsed independent branch identities");
        rejected([&] { (void)direct.reply(sink_left); },
                 "one-shot direct reply allowed replay");
        rejected([&] { (void)direct.reply(answer_right); },
                 "returning frame manufactured a second direct reply");
        rejected([&] { (void)direct.forward(answer_left); },
                 "returned message re-entered downstream forward DAG");
        check(direct.outstanding() == 5, "reply leaked or duplicated credits");
        direct.complete(answer_left);
        direct.complete(answer_right);
        direct.complete(fork[0]);
        direct.complete(fork[1]);
        direct.complete(root);
        check(direct.outstanding() == 0, "direct replies leaked frames");
        direct.close();
    }

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
        retained.close();
    }

    // Metadata credits bound independent route-path copies even when count
    // slots remain. Failed fan-out must be all-or-none and preserve source
    // authority. Return frames must reuse the consumed forward-frame credits.
    {
        ActorPipeRouteLedger measure(stages(), edges(), 40, 8);
        const auto probe = measure.ingress("source");
        const auto source_charge = measure.metadata_bytes();
        const auto probe_child = measure.forward_to(probe, "worker-a");
        const auto worker_charge = measure.metadata_bytes() - source_charge;
        check(source_charge > 0 && worker_charge > source_charge,
              "route metadata path charge did not grow on forwarding");

        ActorPipeRouteLedger budgeted(stages(), edges(), 41, 8,
                                      source_charge + worker_charge);
        const auto root = budgeted.ingress("source");
        check(budgeted.metadata_bytes() == source_charge,
              "ingress metadata was not charged");
        rejected([&] { (void)budgeted.forward_all(root); },
                 "fan-out bypassed whole-graph metadata credits");
        check(budgeted.outstanding() == 1 &&
              budgeted.metadata_bytes() == source_charge,
              "failed fan-out partially published route credits");
        const auto child = budgeted.forward_to(root, "worker-a");
        check(budgeted.outstanding() == 2 &&
              budgeted.metadata_bytes() <= source_charge + worker_charge,
              "single child bypassed metadata cap");
        rejected([&] { (void)budgeted.round_robin(root); },
                 "failed child metadata allocation bypassed live quota");
        const auto reply = budgeted.backward(child);
        check(reply->receiver() == "source" &&
              budgeted.outstanding() == 2 &&
              budgeted.metadata_bytes() <= source_charge + worker_charge,
              "reverse replacement leaked or overcharged metadata");
        rejected([&] { budgeted.complete(child); },
                 "replaced forward frame remained live");
        budgeted.retire_if_active(reply);
        check(budgeted.metadata_bytes() == source_charge,
              "reverse retirement failed to refund bytes");
        check(budgeted.abort_and_advance(42) == 1 &&
              budgeted.metadata_bytes() == 0,
              "generation abort leaked route metadata credits");
        rejected([&] { budgeted.complete(root); },
                 "aborted generation still accepted stale frame");
        const auto fresh = budgeted.ingress("source");
        check(budgeted.metadata_bytes() == source_charge,
              "fresh generation failed to reacquire metadata credits");
        budgeted.close();
        check(budgeted.metadata_bytes() == 0,
              "STOP failed to release all route metadata");

        // Direct reply must atomically replace its forward frame at the
        // same tight metadata limit. Replay/forward of retired handles must
        // be rejected, and completing the reply must release exactly its
        // route-path charge without touching the live ingress frame.
        ActorPipeRouteLedger direct_budget(stages(), edges(), 43, 8,
                                            source_charge + worker_charge);
        const auto direct_root = direct_budget.ingress("source");
        const auto direct_child = direct_budget.forward_to(direct_root, "worker-a");
        const auto direct_reply = direct_budget.reply(direct_child);
        check(direct_reply->receiver() == "source" &&
              direct_reply->returning() &&
              direct_budget.outstanding() == 2 &&
              direct_budget.metadata_bytes() <= source_charge + worker_charge,
              "direct reply exceeded or leaked its replaced frame metadata");
        rejected([&] { (void)direct_budget.reply(direct_child); },
                 "retired forward frame replied twice under byte pressure");
        rejected([&] { (void)direct_budget.forward(direct_reply); },
                 "return frame reentered forward graph under byte pressure");
        direct_budget.complete(direct_reply);
        check(direct_budget.metadata_bytes() == source_charge &&
              direct_budget.outstanding() == 1,
              "direct reply retirement failed to refund path bytes");
        direct_budget.complete(direct_root);
        check(direct_budget.metadata_bytes() == 0 &&
              direct_budget.outstanding() == 0,
              "direct reply budget failed to return to zero");

        rejected([&] {
            ActorPipeRouteLedger invalid(stages(), edges(), 2, 5, 0);
        }, "zero route metadata budget accepted");
        rejected([&] {
            ActorPipeRouteLedger invalid(stages(), edges(), 2, 5,
                                         64 * 1024 * 1024 + 1);
        }, "oversized route metadata budget accepted");
        ActorPipeRouteLedger too_tight(stages(), edges(), 44, 8, 1);
        rejected([&] { (void)too_tight.ingress("source"); },
                 "ingress exceeded metadata budget without rejection");
        check(too_tight.outstanding() == 0 &&
              too_tight.metadata_bytes() == 0,
              "failed ingress leaked metadata credits");
        (void)fresh;
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
