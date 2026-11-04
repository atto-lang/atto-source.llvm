#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

struct Model {
    std::atomic<int> entered{0};
    std::atomic<int> cleanup{0};
    std::atomic<int> errors{0};
    std::atomic<std::uint64_t> worker{0};
    bool overflow = false;
};

static std::uint64_t worker_tag() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(
        std::this_thread::get_id()));
}

static MultiActorCarrier::Outcome dispatch(
        void* raw, MultiActorCarrier::Message message,
        MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& model = *static_cast<Model*>(raw);
    if (message.sequence != 1 || cause != MultiActorCarrier::Cause::initial ||
            fuel != 32) model.errors.fetch_add(1);
    model.worker.store(worker_tag());
    model.entered.fetch_add(1);
    // A compiler-lowered actor would return this status after a checked
    // virtual-frame budget check, BEFORE native stack overflow. Here it is
    // simulated by the host; no guest compiler/stack overflow is involved.
    return model.overflow
        ? MultiActorCarrier::Outcome::stack_budget_exhausted
        : MultiActorCarrier::Outcome::complete;
}

static void cancel(void* raw, MultiActorCarrier::Message message,
                   bool had_active) noexcept {
    auto& model = *static_cast<Model*>(raw);
    if (message.sequence != 1 || !had_active ||
            worker_tag() != model.worker.load()) model.errors.fetch_add(1);
    model.cleanup.fetch_add(1);
}

template<class Predicate>
static bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

int main() {
    Model failing;
    Model healthy;
    failing.overflow = true;
    MultiActorCarrier carrier(32);
    auto a = carrier.register_actor(&failing, dispatch, cancel);
    auto b = carrier.register_actor(&healthy, dispatch, cancel);
    auto sender = carrier.producer();
    carrier.start();

    assert(sender.try_send(a, {1, 0}));
    assert(until([&] { return carrier.actor_stats(a).retired; }));
    auto failure = carrier.actor_stats(a);
    assert(failure.stack_budget_exhausted && failure.retired && !failure.active);
    assert(failure.completed == 0 && failing.entered.load() == 1);
    assert(failing.cleanup.load() == 1 && failing.errors.load() == 0);
    assert(!sender.try_send(a, {2, 0}));
    assert(!sender.request_cancel(a));
    assert(!carrier.actor_stats({a.slot, a.generation + 1}).stack_budget_exhausted);

    // B is dispatched only AFTER A's failed turn and cleanup acknowledgment.
    assert(sender.try_send(b, {1, 0}));
    assert(until([&] { return carrier.actor_stats(b).completed == 1; }));
    auto success = carrier.actor_stats(b);
    assert(success.completed == 1 && !success.stack_budget_exhausted);
    assert(healthy.entered.load() == 1 && healthy.errors.load() == 0);
    assert(failing.worker.load() != 0);
    assert(healthy.worker.load() == failing.worker.load());
    assert(carrier.stats().worker_thread_tag == healthy.worker.load());
    assert(carrier.stats().stack_budget_exhaustions == 1);
    assert(carrier.stats().protocol_failures == 0);

    carrier.join();
    assert(carrier.stopped());
    assert(carrier.actor_stats(a).stack_budget_exhausted);
    assert(carrier.stats().stack_budget_exhaustions == 1);
    return 0;
}
