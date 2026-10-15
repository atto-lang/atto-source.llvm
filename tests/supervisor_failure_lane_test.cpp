#include "oreslang/runtime/SupervisorFailureLane.hpp"

#include <stdexcept>
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while (false)
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
    std::vector<Lane::Kind> origins;
    std::vector<std::uint32_t> origin_codes;
    bool fail_once = false;
    bool fail_after_side_effect_once = false;
    void add(const Lane::Failure& event) {
        if (fail_once) { fail_once = false; throw std::runtime_error("handler refused"); }
        events.push_back(event.kind);
        origins.push_back(event.origin_kind);
        origin_codes.push_back(event.origin_code);
        if (fail_after_side_effect_once) {
            fail_after_side_effect_once = false;
            throw std::runtime_error("handler side effect already occurred");
        }
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
        CHECK(lane.publish({key(i), kinds[i], std::string(1000, 'x')}));
        CHECK(!lane.publish({key(i), kinds[i], "duplicate"}));
    }
    CHECK(lane.pending_count() == 5);
    bool full = false;
    try { lane.reserve(key(99)); }
    catch (const std::logic_error&) { full = true; }
    CHECK(full);

    Recorder handler;
    handler.fail_once = true;
    bool caught = false;
    try { lane.dispatch_one(handler); }
    catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);
    CHECK(lane.pending_count() == 4);
    // The callback may have acted before throwing. Fail closed with
    // at-most-once delivery rather than replaying an ambiguous effect.
    for (int i = 1; i < 5; ++i) CHECK(lane.dispatch_one(handler));
    CHECK(!lane.dispatch_one(handler));
    CHECK(lane.reserved_count() == 0);
    CHECK(handler.events.size() == 4);
    CHECK(handler.events[0] == Lane::Kind::Panic);
    CHECK(handler.events[1] == Lane::Kind::Panic);
    CHECK(handler.events[3] == Lane::Kind::HostFailure);
    CHECK(handler.origins == std::vector<Lane::Kind>({
        Lane::Kind::Raise, Lane::Kind::Panic, Lane::Kind::Cancel,
        Lane::Kind::HostFailure
    }));
    CHECK(handler.origin_codes == std::vector<std::uint32_t>(4, 0u));

    // Deliberately throw *after* mutating host-visible handler state.
    // The failure must be observed exactly once, even if the caller retries
    // dispatch_one on the next carrier admission.
    const auto prior = handler.events.size();
    const auto already_applied = key(65);
    lane.reserve(already_applied);
    CHECK(lane.publish_preallocated(
            already_applied, Lane::Kind::Panic, "effect-before-throw"));
    handler.fail_after_side_effect_once = true;
    bool side_effect_threw = false;
    try { (void)lane.dispatch_one(handler); }
    catch (const std::runtime_error&) { side_effect_threw = true; }
    CHECK(side_effect_threw);
    CHECK(handler.events.size() == prior + 1);
    CHECK(handler.events.back() == Lane::Kind::Panic);
    CHECK(!lane.dispatch_one(handler));
    CHECK(lane.pending_count() == 0 && lane.reserved_count() == 0);
    CHECK(normal_mail.size() == 100000);

    lane.reserve(key(1, 2));
    caught = false;
    try { lane.publish({key(1, 1), Lane::Kind::Panic, "stale"}); }
    catch (const std::logic_error&) { caught = true; }
    CHECK(caught);
    lane.publish({key(1, 2), Lane::Kind::Panic, std::string(1000, 'z')});
    CHECK(lane.poll()->summary.size() == 512);
    lane.retry(key(1, 2));
    CHECK(lane.dispatch_one(handler));
    CHECK(lane.reserved_count() == 0);

    // Native low-allocation panic path uses storage reserved at spawn.
    Lane preallocated(2);
    preallocated.reserve(key(77));
    const std::string oversized(2000, 'p');
    CHECK(preallocated.publish_preallocated(
        key(77), Lane::Kind::Panic, oversized, "daddy-root"));
    CHECK(!preallocated.publish_preallocated(
        key(77), Lane::Kind::Raise, "duplicate"));
    const auto terminal = preallocated.poll();
    CHECK(terminal.has_value());
    CHECK(terminal->summary.size() == 512);
    CHECK(terminal->parent_id == "daddy-root");
    CHECK(terminal->kind == Lane::Kind::Panic);
    CHECK(terminal->origin_kind == Lane::Kind::Panic);
    preallocated.ack(key(77));
    CHECK(preallocated.reserved_count() == 0);

    // Reservation may be reused, but the previous generation is rejected.
    preallocated.reserve(key(77, 2));
    bool stale = false;
    try {
        preallocated.publish_preallocated(
            key(77), Lane::Kind::Panic, "old generation");
    } catch (const std::logic_error&) { stale = true; }
    CHECK(stale);
    preallocated.retire_gracefully(key(77, 2));
    CHECK(preallocated.reserved_count() == 0);

    // Same 512-byte scalar-safe CONTROL envelope as the Java supervisor.
    const std::string rocket = "\xf0\x9f\x9a\xa8";
    Lane unicode_lane(4);
    unicode_lane.reserve(key(88));
    CHECK(unicode_lane.publish_preallocated(
        key(88), Lane::Kind::Panic, std::string(130, 'x') + rocket));
    CHECK(unicode_lane.poll()->summary == std::string(130, 'x') + rocket);
    unicode_lane.ack(key(88));

    unicode_lane.reserve(key(89));
    CHECK(unicode_lane.publish_preallocated(
        key(89), Lane::Kind::Panic, std::string(510, 'x') + rocket));
    CHECK(unicode_lane.poll()->summary == std::string(510, 'x'));
    unicode_lane.ack(key(89));

    unicode_lane.reserve(key(90));
    const auto malformed = std::string("\xed\xa0\x80", 3) + rocket;
    CHECK(unicode_lane.publish_preallocated(
        key(90), Lane::Kind::Panic, malformed, std::string(127, 'p') + rocket));
    const auto event = unicode_lane.poll();
    CHECK(event.has_value());
    CHECK(event->summary == "???" + rocket);
    CHECK(event->parent_id == std::string(127, 'p'));
    unicode_lane.ack(key(90));

    unicode_lane.reserve(key(91));
    bool rejected = false;
    try {
        unicode_lane.publish_preallocated(
            key(91), static_cast<Lane::Kind>(99), "forged");
    } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(unicode_lane.pending_count() == 0);
    CHECK(unicode_lane.publish_preallocated(key(91), Lane::Kind::Panic, "valid"));
    unicode_lane.ack(unicode_lane.poll()->actor);

    // Direct low-level lane writers must normalize unhandled actor
    // Throw/Raise to supervisor-visible Panic and retain provenance.
    Lane normalized(2);
    normalized.reserve(key(101));
    normalized.reserve(key(102));
    CHECK(normalized.publish_preallocated(key(101), Lane::Kind::Throw,
            "first", "parent", std::nullopt, 0xffffffffu));
    CHECK(normalized.publish_preallocated(key(102), Lane::Kind::Raise,
            "second", "parent", std::nullopt, 91u));
    auto thrown = normalized.poll();
    CHECK(thrown && thrown->kind == Lane::Kind::Panic);
    CHECK(thrown->origin_kind == Lane::Kind::Throw);
    CHECK(thrown->origin_code == 0xffffffffu);
    normalized.ack(key(101));
    auto raised = normalized.poll();
    CHECK(raised && raised->kind == Lane::Kind::Panic);
    CHECK(raised->origin_kind == Lane::Kind::Raise);
    CHECK(raised->origin_code == 91u);
    normalized.ack(key(102));

    normalized.reserve(key(103));
    bool mismatched = false;
    try {
        normalized.publish_preallocated(
            key(103), Lane::Kind::Throw, "bad", "parent", Lane::Kind::Raise);
    } catch (const std::invalid_argument&) { mismatched = true; }
    CHECK(mismatched && normalized.pending_count() == 0);
    normalized.retire_gracefully(key(103));

    // Longer at-most-once FIFO stress: injected exceptions after a host
    // handler side effect must not replay the failed child, strand capacity,
    // or prevent later distinct child notifications from being delivered.
    Lane stress(4);
    Recorder stress_handler;
    constexpr int kFailures = 4096;
    for (int i = 0; i < kFailures; ++i) {
        const Lane::ActorKey actor{"stress-" + std::to_string(i), 1};
        stress.reserve(actor);
        CHECK(stress.publish_preallocated(
                actor, Lane::Kind::Panic, "stress terminal", "supervisor"));
        const bool throw_after = (i % 3 == 0);
        stress_handler.fail_after_side_effect_once = throw_after;
        bool escaped = false;
        try { CHECK(stress.dispatch_one(stress_handler)); }
        catch (const std::runtime_error&) { escaped = true; }
        CHECK(escaped == throw_after);
        CHECK(stress_handler.events.size() == static_cast<std::size_t>(i + 1));
        CHECK(stress.pending_count() == 0 && stress.reserved_count() == 0);
        CHECK(!stress.dispatch_one(stress_handler));
    }
    CHECK(stress_handler.events.size() == kFailures);
    return 0;
}
