#include "oreslang/runtime/TrustedOutboundTokenBucket.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using oreslang::runtime::TrustedOutboundTokenBucket;
static void require(bool condition) { if (!condition) std::abort(); }

static void fractional_refill_and_rollback() {
    TrustedOutboundTokenBucket bucket(20, 5, 0);
    require(bucket.valid_policy());
    for (int i = 0; i < 5; ++i) require(bucket.try_consume(1, 0));
    require(!bucket.try_consume(1, 0));
    require(!bucket.try_consume(1, 25000000));
    require(bucket.try_consume(1, 50000000));
    require(!bucket.try_consume(1, 50000000));
    require(bucket.available(49000000) == 0); // forged time rollback
    require(bucket.available(1050000000) == 5);
    require(bucket.available(UINT64_MAX) == 5);
}

static void unsigned_nano_wrap_refills_but_backwards_time_does_not() {
    TrustedOutboundTokenBucket bucket(1000000000ULL, 5, UINT64_MAX - 2);
    require(bucket.try_consume(5, UINT64_MAX - 2));
    // Four forward nanoseconds across the unsigned timestamp wrap.
    require(bucket.available(1) == 4);
    require(bucket.available(0) == 4); // backwards sample
    require(bucket.available(2) == 5);
    require(bucket.available(UINT64_MAX - 1) == 5); // old epoch is not forward
}

static void failed_enqueue_refunds_only_prior_charge() {
    TrustedOutboundTokenBucket bucket(1, 2, 0);
    require(bucket.try_consume(2, 0));
    require(!bucket.try_consume(1, 0));
    require(bucket.rollback_unpublished(2, 0));
    require(bucket.available(0) == 2);
    require(bucket.try_consume(2, 0));
    require(!bucket.try_consume(1, 0));
}

static void concurrent_senders_cannot_overspend() {
    TrustedOutboundTokenBucket bucket(20, 5, 0);
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::atomic<int> admitted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            if (bucket.try_consume(1, 0)) admitted.fetch_add(1);
        });
    }
    while (ready.load() != 16) std::this_thread::yield();
    start.store(true);
    for (auto& t : threads) t.join();
    require(admitted.load() == 5);
    require(bucket.available(0) == 0);
}

static void invalid_policy_or_charge_denied() {
    TrustedOutboundTokenBucket none(0, 5, 0);
    TrustedOutboundTokenBucket huge(1, UINT64_MAX, 0);
    require(!none.valid_policy() && !none.try_consume(1, 0));
    require(!huge.valid_policy() && !huge.try_consume(1, 0));
    TrustedOutboundTokenBucket bucket(1, 5, 0);
    require(!bucket.try_consume(0, 0));
    require(!bucket.try_consume(6, 0));
    require(!bucket.rollback_unpublished(0, 0));
}

int main() {
    fractional_refill_and_rollback();
    unsigned_nano_wrap_refills_but_backwards_time_does_not();
    failed_enqueue_refunds_only_prior_charge();
    concurrent_senders_cannot_overspend();
    invalid_policy_or_charge_denied();
    std::cout << "TRUSTED_OUTBOUND_BUCKET_HOST_ONLY_PASS\n";
    return 0;
}
