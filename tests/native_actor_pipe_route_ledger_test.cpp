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
