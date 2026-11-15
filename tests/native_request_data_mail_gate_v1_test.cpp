#include "oreslang/runtime/NativeRequestDataMailGateV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace oreslang::runtime;

namespace {
struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    TaskCancellation::Ptr cancel_a{TaskCancellation::root()};
    TaskCancellation::Ptr cancel_b{TaskCancellation::root()};
    RequestContextLocalV1::Ptr a, b;
    RequestLocalKeyV1<std::string> trace;
    std::unique_ptr<NativeRequestDataMailGateV1> gate_a, gate_b;
    std::mutex lock;
    std::condition_variable cv;
    int seen = 0;
};

void on_message(void* context, std::size_t index, std::uint64_t payload) {
    auto& s = *static_cast<Scenario*>(context);
    assert(index == 0);
    std::uint64_t current_sequence = 0;
    assert(ores_native_actor_scheduler_current_data_mail_sequence(
        s.scheduler, s.actor, &current_sequence));
    assert(current_sequence == payload); // in this test, 1,2,3
    if (payload == 1) {
        assert(s.gate_a->authorized());
        assert(!s.gate_b->authorized());
        assert(s.gate_a->set(s.trace, std::string("trace-A")));
        assert(!s.gate_b->get_copy(s.trace));
    } else if (payload == 2) {
        assert(!s.gate_a->authorized()); // original mail A is no longer active
        assert(s.gate_b->authorized());
        assert(!s.gate_a->get_copy(s.trace));
        assert(s.gate_b->set(s.trace, std::string("trace-B")));
        assert(s.gate_b->get_copy(s.trace) == std::string("trace-B"));
    } else if (payload == 3) {
        // Same physical actor, new message. No request may inherit this mail.
        assert(!s.gate_a->authorized());
        assert(!s.gate_b->authorized());
        assert(!s.gate_a->get_copy(s.trace));
        assert(!s.gate_b->get_copy(s.trace));
    } else {
        assert(false && "unexpected source DATA mail");
    }
    {
        std::lock_guard<std::mutex> guard(s.lock);
        assert(++s.seen == static_cast<int>(payload));
    }
    s.cv.notify_one();
}

void deliver(Scenario& s, std::uint64_t seq) {
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, seq) == ORES_NATIVE_SEND_ACCEPTED);
    std::unique_lock<std::mutex> guard(s.lock);
    assert(s.cv.wait_for(guard, std::chrono::seconds(5),
                         [&] { return s.seen >= static_cast<int>(seq); }));
}

void test_actual_scheduler_request_gate() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open(1, 8, 1, on_message, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);
    s.a = RequestContextLocalV1::open(
        {3, 4, 501, 1, s.actor}, s.cancel_a);
    s.b = RequestContextLocalV1::open(
        {3, 4, 502, 1, s.actor}, s.cancel_b);
    assert(s.a && s.b);
    s.gate_a = std::make_unique<NativeRequestDataMailGateV1>(
        *s.scheduler, s.a, s.actor, 1);
    s.gate_b = std::make_unique<NativeRequestDataMailGateV1>(
        *s.scheduler, s.b, s.actor, 2);
    // A pooled actor may handle other DATA mails, but an existing request
    // store is pinned forever to its first bound original mailbox receive.
    assert(!s.a->bind_original_mail_once(2));
    bool wrong_mail_rejected = false;
    try {
        NativeRequestDataMailGateV1 wrong(*s.scheduler, s.a, s.actor, 2);
    } catch (const std::invalid_argument&) {
        wrong_mail_rejected = true;
    }
    assert(wrong_mail_rejected);

    deliver(s, 1);
    assert(s.a->get_copy(s.trace) == std::string("trace-A"));
    assert(!s.gate_a->authorized());
    // A callback producer on a different physical thread cannot borrow
    // an old request even while that request remains live.
    std::atomic<bool> foreign_denied{false};
    std::thread foreign([&] {
        foreign_denied.store(
            !s.gate_a->authorized() && !s.gate_a->get_copy(s.trace)
            && !s.gate_b->authorized(), std::memory_order_release);
    });
    foreign.join();
    assert(foreign_denied.load(std::memory_order_acquire));

    deliver(s, 2);
    assert(s.b->get_copy(s.trace) == std::string("trace-B"));
    assert(s.cancel_a->cancel("http-request-A-ended"));
    assert(!s.a->get_copy(s.trace));
    assert(s.b->get_copy(s.trace) == std::string("trace-B"));

    deliver(s, 3);
    assert(!s.gate_a->authorized() && !s.gate_b->authorized());
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    assert(!s.gate_b->authorized());
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    s.gate_a.reset();
    s.gate_b.reset();
    s.b->close();
    s.cancel_a->retire();
    s.cancel_b->retire();
}
} // namespace

int main() {
    test_actual_scheduler_request_gate();
    std::puts("NATIVE_REQUEST_DATA_MAIL_GATE_REAL_SCHEDULER_PASS");
}
