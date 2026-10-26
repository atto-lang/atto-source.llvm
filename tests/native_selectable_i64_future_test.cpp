#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableI64Future.hpp"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <cstdint>
#include <stdexcept>

using oreslang::runtime::NativeSelectArbiter;
using oreslang::runtime::NativeSelectableI64Future;
using oreslang::runtime::NativeActorGenerationFence;

static void selected_future_resolves_once() {
    NativeSelectableI64Future<4> future;
    NativeSelectArbiter<1> select;
    auto ready=select.register_case(0);
    NativeSelectableI64Future<4>::Receipt receipt;
    auto token=future.subscribe_ready(
        [&](const NativeSelectableI64Future<4>::Subscription& subscription) {
            assert(ready.try_win_if([&] {
                return future.try_commit_ready(subscription,receipt);
            }));
        });
    ready.arm([token] { (void)token.close(); });
    assert(future.pending_waiters()==1);
    assert(future.resolve(42));
    assert(!future.resolve(43));
    assert(select.winner().has_value());
    assert(receipt.status==NativeSelectableI64Future<4>::Status::Resolved);
    assert(receipt.value==42);
    assert(future.pending_waiters()==0);
}

static void rejection_and_cancellation_have_distinct_receipts() {
    for(int kind=0;kind<2;++kind) {
        NativeSelectableI64Future<2> future;
        NativeSelectArbiter<1> select;
        auto ready=select.register_case(0);
        NativeSelectableI64Future<2>::Receipt receipt;
        auto token=future.subscribe_ready(
            [&](const NativeSelectableI64Future<2>::Subscription& subscription) {
                assert(ready.try_win_if([&] {
                    return future.try_commit_ready(subscription,receipt);
                }));
            });
        ready.arm([token] { (void)token.close(); });
        if(kind==0) {
            assert(future.reject(std::make_exception_ptr(std::runtime_error("fail"))));
            assert(receipt.status==NativeSelectableI64Future<2>::Status::Rejected);
            assert(bool(receipt.error));
        } else {
            assert(future.cancel());
            assert(receipt.status==NativeSelectableI64Future<2>::Status::Cancelled);
            assert(!receipt.error);
        }
    }
}

static void cancelled_loser_not_awakened_by_future() {
    NativeSelectableI64Future<2> future;
    NativeSelectArbiter<1> select;
    auto ready=select.register_case(0);
    int callbacks=0;
    auto token=future.subscribe_ready(
        [&](const NativeSelectableI64Future<2>::Subscription&) { ++callbacks; });
    ready.arm([token] { (void)token.close(); });
    assert(select.cancel());
    assert(future.pending_waiters()==0);
    assert(future.resolve(9));
    assert(callbacks==0);
}

static void stale_actor_generation_cannot_select_future() {
    NativeActorGenerationFence fence;
    auto lease=fence.capture();
    NativeSelectableI64Future<2> future;
    NativeSelectArbiter<1> select;
    auto ready=select.register_case(0);
    NativeSelectableI64Future<2>::Receipt receipt;
    auto token=future.subscribe_ready(
        [&](const NativeSelectableI64Future<2>::Subscription& subscription) {
            (void)ready.try_win_if([&] {
                return lease.current() &&
                    future.try_commit_ready(subscription,receipt);
            });
        });
    ready.arm([token] { (void)token.close(); });
    assert(fence.retire());
    assert(!lease.current());
    assert(future.resolve(77));
    assert(!select.winner().has_value());
    assert(receipt.status==NativeSelectableI64Future<2>::Status::Pending);
    assert(fence.physically_retired());
    assert(select.cancel());
}

static void physical_retirement_waits_for_continuation() {
    NativeActorGenerationFence fence;
    auto token=fence.capture();
    assert(token.run_if_current([&] {
        assert(fence.retire());
        assert(!fence.physically_retired());
    }));
    assert(fence.physically_retired());
    bool ran=false;
    assert(!token.run_if_current([&] { ran=true; }));
    assert(!ran);
}

static void notification_failure_does_not_unresolve_future() {
    NativeSelectableI64Future<2> future;
    auto subscription=future.subscribe_ready(
        [](const NativeSelectableI64Future<2>::Subscription&) {
            throw std::runtime_error("observer failed");
        });
    assert(future.resolve(5));
    assert(future.status()==NativeSelectableI64Future<2>::Status::Resolved);
    assert(bool(future.notification_failure()));
    assert(subscription.close());
}


static void concurrent_resolution_and_physical_retirement_stress() {
    using Future = NativeSelectableI64Future<16>;
    for (int iteration = 0; iteration < 100; ++iteration) {
        Future future;
        std::atomic<int> callbacks{0};
        std::vector<Future::Subscription> waiters;
        for (int i = 0; i < 8; ++i) {
            waiters.push_back(future.subscribe_ready(
                [&](const Future::Subscription& token) {
                    Future::Receipt receipt;
                    if (future.try_commit_ready(token, receipt)) {
                        assert(receipt.status == Future::Status::Resolved);
                        ++callbacks;
                    }
                }));
        }
        std::atomic<int> winners{0};
        std::thread left([&] { if (future.resolve(41)) ++winners; });
        std::thread right([&] { if (future.resolve(42)) ++winners; });
        left.join();
        right.join();
        assert(winners == 1);
        assert(callbacks == 8);
        assert(future.pending_waiters() == 0);
        for (const auto& waiter : waiters) assert(!waiter.active());
    }

    for (int iteration = 0; iteration < 100; ++iteration) {
        NativeActorGenerationFence fence;
        auto token = fence.capture();
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::thread worker([&] {
            assert(token.run_if_current([&] {
                entered = true;
                while (!release.load()) std::this_thread::yield();
            }));
        });
        while (!entered.load()) std::this_thread::yield();
        assert(fence.retire());
        assert(!fence.physically_retired());
        release = true;
        worker.join();
        assert(fence.physically_retired());
        assert(!token.current());
    }
}

int main() {
    selected_future_resolves_once();
    rejection_and_cancellation_have_distinct_receipts();
    cancelled_loser_not_awakened_by_future();
    stale_actor_generation_cannot_select_future();
    physical_retirement_waits_for_continuation();
    notification_failure_does_not_unresolve_future();
    concurrent_resolution_and_physical_retirement_stress();
    return 0;
}
