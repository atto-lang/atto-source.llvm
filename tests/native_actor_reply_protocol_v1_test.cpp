#include "oreslang/runtime/NativeActorReplyStreamsV1.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

using Stream = oreslang::runtime::NativeActorReplyStreamsV1<std::uint64_t>;
template <class F> void denied(F f) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    assert(caught);
}
int main() {
    denied([] { Stream invalid(0, 2); });
    denied([] { Stream invalid(2, 0); });
    denied([] { Stream invalid(257, 256); });
    Stream streams(2, 2);
    denied([&] { (void)streams.endpoint(static_cast<Stream::Role>(99)); });
    auto actor = streams.endpoint(Stream::Role::Actor);
    auto worker = streams.endpoint(Stream::Role::Actor);
    auto app_service = streams.endpoint(Stream::Role::SupervisorApplication);
    auto a = streams.reserve(actor, worker);
    auto b = streams.reserve(actor, app_service);
    denied([&] { (void)a.try_send(app_service, 5); });  // cannot impersonate worker
    denied([&] { (void)a.poll(worker); });             // only caller may consume
    denied([&] { (void)streams.reserve(actor, actor); });
    denied([&] { (void)streams.reserve(actor, worker); }); // slots bounded
    assert(a.poll(actor).status == Stream::ReadStatus::Pending);
    assert(a.try_send(worker, 1) == Stream::WriteStatus::Written);
    assert(a.try_send(worker, 2) == Stream::WriteStatus::Written);
    assert(a.try_send(worker, 3) == Stream::WriteStatus::Full);
    assert(b.try_send(app_service, 99) == Stream::WriteStatus::Written);
    assert(b.fail_stream(app_service) == Stream::WriteStatus::Written);
    assert(b.poll(actor).value == 99); // queued accepted value precedes failure
    assert(b.poll(actor).status == Stream::ReadStatus::Failed);
    assert(b.poll(actor).status == Stream::ReadStatus::Stale);
    assert(a.poll(actor).value == 1);
    assert(a.try_send(worker, 3) == Stream::WriteStatus::Written);
    assert(a.close_stream(worker) == Stream::WriteStatus::Written);
    assert(a.try_send(worker, 4) == Stream::WriteStatus::Terminal);
    assert(a.close_stream(worker) == Stream::WriteStatus::Terminal);
    assert(a.poll(actor).value == 2);
    assert(a.poll(actor).value == 3);
    assert(a.poll(actor).status == Stream::ReadStatus::End);
    assert(streams.pending_count() == 0);
    assert(a.poll(actor).status == Stream::ReadStatus::Stale);

    auto stale = streams.reserve(actor, worker);
    const auto old_handle = stale.local_handle();
    assert(stale.cancel(actor));
    auto fresh = streams.reserve(actor, worker);
    assert(old_handle != fresh.local_handle());
    assert(stale.try_send(worker, 7) == Stream::WriteStatus::Stale);
    assert(fresh.try_send(worker, 8) == Stream::WriteStatus::Written);
    streams.retire(worker);
    assert(fresh.poll(actor).status == Stream::ReadStatus::Stale);
    assert(streams.pending_count() == 0);
    denied([&] { (void)streams.reserve(actor, worker); });

    // Concurrent close/cancel and producer writes arbitrate under one lock.
    auto restarted = streams.endpoint(Stream::Role::Actor);
    std::atomic<int> wins{0};
    for (int i = 0; i < 200; ++i) {
        auto lease = streams.reserve(actor, restarted);
        std::thread producer([&] {
            if (lease.close_stream(restarted) == Stream::WriteStatus::Written) ++wins;
        });
        std::thread canceller([&] { if (lease.cancel(actor)) ++wins; });
        producer.join(); canceller.join();
        assert(streams.pending_count() == 0);
        assert(lease.poll(actor).status == Stream::ReadStatus::Stale);
    }
    assert(wins >= 200 && wins <= 400); // close and cancel may both win in order
    streams.close();
    denied([&] { (void)streams.reserve(actor, restarted); });

    // A leaked handle never reaches a freed owner registry.
    Stream::Lease orphan = [&] {
        Stream temp(1, 1);
        auto one = temp.endpoint(Stream::Role::Actor);
        auto two = temp.endpoint(Stream::Role::Actor);
        return temp.reserve(one, two);
    }();
    assert(orphan.local_handle() != 0);
}
