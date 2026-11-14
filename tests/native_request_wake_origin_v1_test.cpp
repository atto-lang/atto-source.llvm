#include "oreslang/runtime/NativeRequestWakeGateV1.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

using namespace oreslang::runtime;

namespace {
struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    TaskCancellation::Ptr cancellation{TaskCancellation::root()};
    RequestContextLocalV1::Ptr requests[2];
    RequestLocalKeyV1<std::string> trace;
    OresNativeWakeTicket tickets[2]{};
    std::unique_ptr<NativeRequestWakeGateV1> leases[2];
    std::mutex mutex;
    std::condition_variable cv;
    int admitted = 0;
    int resumed = 0;
};

void on_message(void* userdata, std::size_t actor_index, std::uint64_t message) {
    auto& s = *static_cast<Scenario*>(userdata);
    assert(actor_index == 0);
    assert(message == 1 || message == 2);
    const auto ordinal = static_cast<int>(message);
    NativeRequestDataMailGateV1 current(
        *s.scheduler, s.requests[ordinal-1], s.actor, message);
    assert(current.authorized());
    assert(current.set(s.trace, std::string("original-request")));
    OresNativeWakeTicket issued{};
    assert(ores_native_actor_scheduler_reserve_wake(
        s.scheduler, s.actor, s.actor, message, 1, 1, 1, &issued));
    auto lease = NativeRequestWakeGateV1::capture(current, issued);
    assert(lease && !lease->authorized()); // WAKE not yet delivered
    if (ordinal == 2) {
        OresNativeWakeTicket old_ticket{};
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            old_ticket = s.tickets[0];
        }
        // First wake remains RESERVED. Same actor and tenant, but this
        // DIFFERENT DATA mail cannot rebind that wake to request #2.
        assert(!NativeRequestWakeGateV1::capture(current, old_ticket));
    }
    OresNativeWakeTicket forged = issued;
    forged.producer.generation++;
    assert(!NativeRequestWakeGateV1::capture(current, forged));
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.tickets[ordinal-1] = issued;
        s.leases[ordinal-1] = std::move(lease);
        s.admitted = ordinal;
    }
    s.cv.notify_one();
}

void on_wake(void* userdata, std::size_t actor_index,
             OresNativeWakeTicket issued, OresNativeWakeOutcome outcome) {
    auto& s = *static_cast<Scenario*>(userdata);
    assert(actor_index == 0);
    const int index = static_cast<int>(issued.continuation_slot) - 1;
    assert(index == 0 || index == 1);
    assert(ores_native_actor_scheduler_is_current_wake(s.scheduler, issued));
    std::uint64_t sentinel = 999;
    assert(!ores_native_actor_scheduler_current_data_mail_sequence(
        s.scheduler, s.actor, &sentinel));
    assert(sentinel == 999); // WAKE never inherits the last physical DATA mail
    if (index == 0) {
        assert(outcome == ORES_NATIVE_WAKE_READY);
        assert(s.leases[0]->authorized());
        assert(!s.leases[1]->authorized()); // other request's wake is unrelated
        assert(s.leases[0]->get_copy(s.trace)
                == std::string("original-request"));
        assert(s.leases[0]->set(s.trace, std::string("resumed-ready")));
    } else {
        assert(outcome == ORES_NATIVE_WAKE_CANCELLED);
        assert(!s.leases[1]->authorized()); // cancelled Task cannot borrow request
        assert(!s.leases[1]->get_copy(s.trace));
        assert(!s.leases[0]->authorized()); // earlier WAKE ticket is stale
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.resumed;
    }
    s.cv.notify_one();
}

void wait_counter(Scenario& s, int target, bool is_wake) {
    std::unique_lock<std::mutex> lock(s.mutex);
    assert(s.cv.wait_for(lock, std::chrono::seconds(8), [&] {
        return (is_wake ? s.resumed : s.admitted) >= target;
    }));
}

void exercise_ready_cancelled_and_stale_wakes() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 8, 1, on_message, nullptr, nullptr, on_wake, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);
    s.requests[0] = RequestContextLocalV1::open(
        {3, 4, 701, 1, s.actor}, s.cancellation);
    s.requests[1] = RequestContextLocalV1::open(
        {3, 4, 702, 1, s.actor}, s.cancellation);
    assert(s.requests[0] && s.requests[1]);

    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, 1) == ORES_NATIVE_SEND_ACCEPTED);
    wait_counter(s, 1, false);
    assert(!s.leases[0]->authorized()); // external producer thread
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, 2) == ORES_NATIVE_SEND_ACCEPTED);
    wait_counter(s, 2, false);
    assert(s.requests[1]->get_copy(s.trace) == std::string("original-request"));
    assert(s.requests[0]->get_copy(s.trace) == std::string("original-request"));
    // Both wake credits are outstanding, captured from distinct DATA mails.
    assert(ores_native_actor_scheduler_transfer_wake(
        s.scheduler, s.tickets[0]));
    assert(ores_native_actor_scheduler_publish_wake(
        s.scheduler, s.tickets[0], ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    wait_counter(s, 1, true);
    assert(s.requests[0]->get_copy(s.trace) == std::string("resumed-ready"));
    assert(!s.requests[1]->get_copy(s.trace));
    assert(!s.leases[0]->authorized());
    assert(s.requests[1]->get_copy(s.trace) == std::string("original-request"));
    assert(ores_native_actor_scheduler_transfer_wake(
        s.scheduler, s.tickets[1]));
    assert(ores_native_actor_scheduler_publish_wake(
        s.scheduler, s.tickets[1], ORES_NATIVE_WAKE_CANCELLED)
        == ORES_NATIVE_WAKE_PUBLISHED);
    wait_counter(s, 2, true);
    assert(!s.leases[1]->authorized());
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    s.leases[0].reset();
    s.leases[1].reset();
    assert(s.requests[0]->close());
    assert(s.requests[1]->close());
    s.cancellation->retire();
}
} // namespace

int main() {
    exercise_ready_cancelled_and_stale_wakes();
    std::puts("NATIVE_REQUEST_WAKE_ORIGIN_READY_CANCELLED_PASS");
}
