#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

namespace {
using Context = RequestContextLocalV1;
thread_local int test_native_tls_marker = 0;

struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    RequestLocalKeyV1<std::string> trace;
    RequestLocalKeyV1<int> counter;
    TaskCancellation::Ptr left_cancel{TaskCancellation::root()};
    TaskCancellation::Ptr right_cancel{TaskCancellation::root()};
    Context::Ptr left, right;
    std::mutex mutex;
    std::condition_variable cv;
    int turns = 0;
    bool correct = true;
};

void on_actor_message(void* userdata, std::size_t actor_index,
                      std::uint64_t payload) {
    auto& s = *static_cast<Scenario*>(userdata);
    assert(actor_index == 0);
    assert(ores_native_actor_scheduler_is_current_carrier(s.scheduler));
    assert(ores_native_actor_scheduler_is_current_actor(
        s.scheduler, s.actor));
    assert(s.left->belongs_to(s.actor) && s.right->belongs_to(s.actor));
    // Two overlapping requests have the SAME actor, but different local
    // storage. A physical TLS marker is intentionally shared across turns.
    if (payload == 1) {
        assert(test_native_tls_marker == 0);
        test_native_tls_marker = 41;
        assert(s.left->set(s.trace, std::string("trace-left")));
        assert(s.left->set(s.counter, 1));
    } else if (payload == 2) {
        assert(test_native_tls_marker == 41);
        assert(!s.right->get_copy(s.trace));
        assert(s.right->set(s.trace, std::string("trace-right")));
        assert(s.right->set(s.counter, 2));
    } else if (payload == 3) {
        assert(test_native_tls_marker == 41);
        assert(s.left->get_copy(s.trace) == std::string("trace-left"));
        assert(s.left->get_copy(s.counter) == 1);
        assert(s.right->get_copy(s.trace) == std::string("trace-right"));
    } else if (payload == 4) {
        assert(s.right->get_copy(s.trace) == std::string("trace-right"));
        assert(s.right->get_copy(s.counter) == 2);
        assert(s.left->get_copy(s.trace) == std::string("trace-left"));
    } else {
        std::terminate();
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        assert(++s.turns == static_cast<int>(payload));
    }
    s.cv.notify_one();
}

void deliver(Scenario& s, int turn) {
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, static_cast<std::uint64_t>(turn))
        == ORES_NATIVE_SEND_ACCEPTED);
    std::unique_lock<std::mutex> lock(s.mutex);
    assert(s.cv.wait_for(lock, std::chrono::seconds(8),
                         [&] { return s.turns >= turn; }));
}

void test_same_actor_two_simultaneous_requests() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open(
        1, 4, 1, on_actor_message, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);

    const RequestIdentityV1 a{11, 22, 501, 1, s.actor};
    const RequestIdentityV1 b{11, 22, 502, 1, s.actor};
    s.left = Context::open(a, s.left_cancel);
    s.right = Context::open(b, s.right_cancel);
    assert(s.left && s.right);
    assert(s.left->identity().request != s.right->identity().request);
    assert(s.left->identity().generation == 1);
    assert(!s.left->belongs_to({s.actor.scheduler_domain,
                                s.actor.actor_slot,
                                s.actor.generation + 1}));

    deliver(s, 1);
    deliver(s, 2);
    deliver(s, 3);
    deliver(s, 4);

    // Cancelling a single request must not cancel the shared actor or the
    // other request's context.
    assert(s.left_cancel->cancel("http-request-disconnected"));
    assert(!s.left->active());
    assert(!s.left->get_copy(s.trace));
    assert(!s.left->set(s.trace, std::string("stale")));
    assert(s.right->active());
    assert(s.right->get_copy(s.trace) == std::string("trace-right"));

    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    assert(s.right->close());
    assert(!s.right->close());
    assert(!s.right->active());
    s.left_cancel->retire();
    s.right_cancel->retire();
}

void test_context_moves_between_physical_threads() {
    auto authority = TaskCancellation::root();
    const OresNativeActorRef actor{7, 1, 9};
    auto context = Context::open({1, 2, 3, 4, actor}, authority);
    assert(context);
    RequestLocalKeyV1<int> key;
    RequestLocalKeyV1<std::string> label;
    std::atomic<int> phase{0};
    std::thread worker_a([&] {
        test_native_tls_marker = 17;
        assert(context->set(key, 100));
        assert(context->set(label, std::string("scope-is-logical")));
        phase.store(1, std::memory_order_release);
    });
    worker_a.join();
    std::thread worker_b([&] {
        // A new physical thread starts with independent TLS, but reuses the
        // SAME explicit logical request handle (as a resumed Task would).
        assert(test_native_tls_marker == 0);
        assert(phase.load(std::memory_order_acquire) == 1);
        assert(context->get_copy(key) == 100);
        assert(context->get_copy(label) ==
               std::string("scope-is-logical"));
        test_native_tls_marker = 93;
        assert(context->set(key, 101));
    });
    worker_b.join();
    assert(test_native_tls_marker == 0);  // independent host thread TLS
    assert(context->get_copy(key) == 101);
    assert(authority->cancel("request-end"));
    assert(!context->get_copy(key));
    assert(!context->set(key, 102));
    authority->retire();
}

void test_capacity_and_replay_fail_closed() {
    auto authority = TaskCancellation::root();
    assert(!Context::open({}, authority));
    assert(!Context::open({1, 2, 3, 4, {5, 1, 9}}, {}));
    auto scope = Context::open({1, 2, 3, 4, {5, 1, 9}}, authority);
    assert(scope);
    std::vector<RequestLocalKeyV1<int>> keys;
    for (int index = 0; index < 33; ++index) keys.emplace_back();
    for (int index = 0; index < 32; ++index)
        assert(scope->set(keys[static_cast<std::size_t>(index)], index));
    assert(!scope->set(keys[32], 999));
    assert(scope->get_copy(keys[31]) == 31);
    assert(scope->set(keys[0], 123)); // replacement does not use extra slot
    assert(scope->get_copy(keys[0]) == 123);
    assert(scope->close());
    assert(!scope->get_copy(keys[0]));
    assert(!scope->set(keys[0], 2));
    authority->retire();
}

void test_cancel_and_set_race() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto authority = TaskCancellation::root();
        auto scope = Context::open(
            {1, 1, static_cast<std::uint64_t>(iteration + 1), 1,
             {1, 1, 1}}, authority);
        assert(scope);
        RequestLocalKeyV1<int> key;
        std::atomic<bool> begin{false};
        std::thread writer([&] {
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield(); // test worker, not carrier
            for (int n = 0; n != 200; ++n) {
                if (!scope->set(key, n)) break;
                (void)scope->get_copy(key);
            }
        });
        begin.store(true, std::memory_order_release);
        assert(authority->cancel("cancel-concurrently"));
        writer.join();
        assert(!scope->active());
        assert(!scope->get_copy(key));
        assert(!scope->set(key, 999));
        authority->retire();
    }
}
} // namespace

int main() {
    test_same_actor_two_simultaneous_requests();
    test_context_moves_between_physical_threads();
    test_capacity_and_replay_fail_closed();
    test_cancel_and_set_race();
    std::puts("REQUEST_CONTEXT_LOCAL_HOST_ONLY_PASS");
}
