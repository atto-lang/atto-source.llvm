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

    // Parent + child frames cannot acquire new authority after a concurrent
    // supervisor close barrier. Requests that raced *before* close may
    // complete or fail; requests made after close returns must always fail.
    ActorPipeRouteLedger retiring(stages(), edges(), 11, 4096);
    const auto root = retiring.ingress("source");
    std::atomic<bool> begin{false}, after_stop{false};
    std::atomic<int> ready{0}, early{0}, denied{0}, unexpected{0};
    std::vector<std::thread> contenders;
    for (int t=0; t<8; ++t) {
        contenders.emplace_back([&] {
            ++ready;
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!after_stop.load(std::memory_order_acquire)) {
                try {
                    const auto child = retiring.round_robin(root);
                    retiring.complete(child);
                } catch (const std::logic_error&) {
                    // STOP may have linearized between select and complete.
                }
                ++early;
            }
            for (int i=0; i<1000; ++i) {
                try {
                    const auto child = retiring.round_robin(root);
                    retiring.complete(child);
                    ++unexpected;
                } catch (const std::logic_error&) {
                    ++denied;
                }
            }
        });
    }
    while (ready.load() < 8) std::this_thread::yield();
    begin.store(true, std::memory_order_release);
    while (early.load() < 800) std::this_thread::yield();
    retiring.close();
    after_stop.store(true, std::memory_order_release);
    for (auto& contender : contenders) contender.join();
    check(denied == 8000 && unexpected == 0 && retiring.outstanding() == 0,
          "post-STOP ledger selection admitted a stale route frame");
    rejected([&] { retiring.advance_generation(12); },
             "closed correlation ledger reopened through generation bump");
    retiring.close();
    std::cout << "ORES_NATIVE_ACTOR_ROUTE_LEDGER_OK branches=2000"
              << " post_stop_denials=8000" << std::endl;
}
