#include "oreslang/runtime/SupervisorFailureLane.hpp"

#include <cassert>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

using Lane = oreslang::SupervisorFailureLane;

namespace {
Lane::ActorKey key(int id, std::uint64_t generation = 1) {
    return {"child-" + std::to_string(id), generation};
}

struct Recorder final : Lane::Handler {
    std::vector<Lane::Kind> events;
    bool fail_once = false;
    void add(const Lane::Failure& event) {
        if (fail_once) { fail_once = false; throw std::runtime_error("retry"); }
        events.push_back(event.kind);
    }
    void on_throw(const Lane::Failure& event) override { add(event); }
    void on_raise(const Lane::Failure& event) override { add(event); }
    void on_panic(const Lane::Failure& event) override { add(event); }
    void on_cancel(const Lane::Failure& event) override { add(event); }
    void on_host_failure(const Lane::Failure& event) override { add(event); }
};
}

int main() {
    Lane lane(5);
    std::deque<int> normal_mail;
    for (int i = 0; i < 100000; ++i) normal_mail.push_back(i);

    const Lane::Kind kinds[] = {
        Lane::Kind::Throw, Lane::Kind::Raise, Lane::Kind::Panic,
        Lane::Kind::Cancel, Lane::Kind::HostFailure
    };
    for (int i = 0; i < 5; ++i) {
        lane.reserve(key(i));
        assert(lane.publish({key(i), kinds[i], std::string(1000, 'x')}));
        assert(!lane.publish({key(i), kinds[i], "duplicate"}));
    }
    assert(lane.pending_count() == 5);
    bool full = false;
    try { lane.reserve(key(99)); }
    catch (const std::logic_error&) { full = true; }
    assert(full);

    Recorder handler;
    handler.fail_once = true;
    bool caught = false;
    try { lane.dispatch_one(handler); }
    catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    assert(lane.pending_count() == 5);
    // Failure was requeued behind the remaining CONTROL events.
    for (int i = 1; i < 5; ++i) assert(lane.dispatch_one(handler));
    assert(lane.dispatch_one(handler));
    assert(!lane.dispatch_one(handler));
    assert(lane.reserved_count() == 0);
    assert(handler.events.size() == 5);
    assert(handler.events[0] == Lane::Kind::Raise);
    assert(handler.events[4] == Lane::Kind::Throw);
    assert(normal_mail.size() == 100000);

    lane.reserve(key(1, 2));
    caught = false;
    try { lane.publish({key(1, 1), Lane::Kind::Panic, "stale"}); }
    catch (const std::logic_error&) { caught = true; }
    assert(caught);
    lane.publish({key(1, 2), Lane::Kind::Panic, std::string(1000, 'z')});
    assert(lane.poll()->summary.size() == 512);
    lane.retry(key(1, 2));
    assert(lane.dispatch_one(handler));
    assert(lane.reserved_count() == 0);

    return 0;
}
