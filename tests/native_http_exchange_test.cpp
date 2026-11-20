#include "oreslang/runtime/NativeHttpExchange.hpp"
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeHttpExchange;

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

static bool same_actor(
        OresNativeActorRef left,
        OresNativeActorRef right) noexcept {
    return left.scheduler_domain == right.scheduler_domain
        && left.actor_slot == right.actor_slot
        && left.generation == right.generation;
}

int main() {
    NativeHttpExchange exchange;

    const OresNativeActorRef owner{11, 7, 3};
    OresNativeActorRef replacement = owner;
    ++replacement.generation;
    OresNativeActorRef wrong_domain = owner;
    ++wrong_domain.scheduler_domain;
    OresNativeActorRef wrong_slot = owner;
    ++wrong_slot.actor_slot;

    CHECK(!exchange.enqueue({0, 7, 3}));
    CHECK(!exchange.enqueue({11, 7, 0}));

    CHECK(exchange.enqueue(owner));
    auto first = exchange.token();
    CHECK(same_actor(first.actor, owner));
    CHECK(!exchange.enqueue(replacement));

    CHECK(!exchange.claim({first.generation, replacement}));
    CHECK(!exchange.claim({first.generation, wrong_domain}));
    CHECK(!exchange.claim({first.generation, wrong_slot}));
    CHECK(exchange.rollback(first));

    // Reuse the same logical actor slot with a different actor generation.
    // The new exchange generation alone must not let the retired incarnation
    // claim authority over the replacement actor's request.
    CHECK(exchange.enqueue(replacement));
    auto second = exchange.token();
    CHECK(same_actor(second.actor, replacement));
    CHECK(!exchange.claim(first));
    CHECK(!exchange.claim({second.generation, owner}));
    CHECK(exchange.claim(second));
    CHECK(!exchange.claim(second));
    CHECK(!exchange.reclaim(second));
    CHECK(exchange.complete(second));
    CHECK(!exchange.complete(second));
    CHECK(exchange.reclaim(second));

    int request = 0;
    int response = 0;
    for (int n = 1; n <= 1000; ++n) {
        request = n;
        CHECK(exchange.enqueue(replacement));
        auto token = exchange.token();
        std::thread worker([&] {
            CHECK(exchange.claim(token));
            response = request * 2;
            CHECK(exchange.complete(token));
        });
        while (exchange.phase() != NativeHttpExchange::Phase::completed)
            std::this_thread::yield();
        CHECK(exchange.reclaim(token));
        CHECK(response == n * 2);
        worker.join();
    }
}
