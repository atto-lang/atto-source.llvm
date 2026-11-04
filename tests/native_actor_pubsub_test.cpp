#include "oreslang/runtime/NativeActorPubSub.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorPubSubAdmission;
using oreslang::runtime::NativeActorPubSubResourceLedger;

template<class Bus, class = void>
struct HasUnscopedSystemPublish : std::false_type {};

template<class Bus>
struct HasUnscopedSystemPublish<Bus, std::void_t<
        decltype(std::declval<Bus&>().publish_system(
                std::declval<std::uint64_t>(),
                std::declval<const std::string&>(),
                std::declval<const std::string&>()))>> : std::true_type {};

static_assert(!HasUnscopedSystemPublish<NativeActorPubSubAdmission<std::string>>::value,
              "untrusted topic admission must not expose system publication");

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

template<class Fn>
static bool rejected(Fn fn) {
    try { fn(); } catch (const std::logic_error&) { return true; }
    return false;
}

int main() {
    NativeActorMembershipRegistry memberships(90, 8, 4);
    auto a = memberships.create_group(1, 8);
    auto b = memberships.create_group(2, 8);

    for (std::uint64_t id = 10; id <= 13; ++id)
        check(memberships.register_actor(id, 1), "register actor");

    check(memberships.join(a, 10, 1), "join publisher A");
    check(memberships.join(a, 11, 1), "join subscriber 11 A");
    check(memberships.join(a, 12, 1), "join subscriber 12 A");
    check(memberships.join(b, 11, 1), "join subscriber 11 B");

    NativeActorPubSubAdmission<std::string> bus(memberships, 8, 4);
    // Portable conformance PS018 and group-scoped PS021–PS026 require
    // exact ASCII names, not permissive native UTF-8 / locale admission.
    // The C ABI must share the same validation function.
    {
        using oreslang::runtime::valid_portable_topic_name;
        NativeActorPubSubAdmission<std::string> names(memberships, 16, 4);
        for (const std::string& good : {
                std::string("a"), std::string("A"),
                std::string("orders.created/v1-2_3"),
                std::string(128, 'a')}) {
            check(valid_portable_topic_name(good),
                  "portable valid topic rejected by shared validator");
            names.define_topic(a.group_id(), good,
                NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable, 1);
        }
        for (const std::string& bad : {
                std::string(), std::string("."), std::string("_"),
                std::string("9topic"), std::string("bad space"),
                std::string(" bad"), std::string("bad\tname"),
                std::string("bad:name"), std::string("bad\\name"),
                std::string(129, 'a'), std::string("a\0z", 3),
                std::string("\xC3\xA9"), std::string("a\xC2\xA0" "b"),
                std::string("\xFF", 1)}) {
            check(!valid_portable_topic_name(bad),
                  "invalid portable topic admitted by shared validator");
            bool invalid = false;
            try {
                names.define_topic(a.group_id(), bad,
                    NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable, 1);
            } catch (const std::invalid_argument&) { invalid = true; }
            check(invalid, "native topic admitted nonportable name");
        }
        check(names.close_group(a.group_id()) == 4,
              "rejected topic names consumed capacity or leaked state");
    }
    bool c1_control_rejected = false;
    try {
        bus.define_topic(
            a.group_id(), "bad-\xC2\x85",
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable, 1);
    } catch (const std::invalid_argument&) {
        c1_control_rejected = true;
    }
    check(c1_control_rejected,
          "typed bus accepted a Unicode C1-control topic name");
    // UTF-8 whitespace-only topic keys must not differ from other backends.
    for (const auto& invalid_name : {
            std::string(" "), std::string("  "),
            std::string("\xC2\xA0"), std::string("\xE1\x9A\x80"),
            std::string("\xE2\x80\x83"), std::string("\xE2\x80\xAF"),
            std::string("\xE3\x80\x80")}) {
        check(rejected([&] {
            bus.define_topic(a.group_id(), invalid_name,
                    NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Lossy, 1);
        }), "Unicode whitespace-only native topic was admitted");
    }

    bus.define_topic(
            a.group_id(), "reliable",
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable,
            1);
    bus.define_topic(
            a.group_id(), "latest",
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Latest,
            1);
    bus.define_topic(
            b.group_id(), "lossy",
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Lossy,
            1);

    check(bus.subscribe(a.group_id(), "reliable", 11, 1), "subscribe 11 reliable");
    check(bus.subscribe(a.group_id(), "reliable", 12, 1), "subscribe 12 reliable");
    check(!bus.subscribe(a.group_id(), "reliable", 12, 1), "duplicate subscription not idempotent");
    check(rejected([&] { (void)bus.subscribe(a.group_id(), "reliable", 13, 1); }),
          "non-member subscribed");

    auto first = bus.publish(a.group_id(), "reliable", 10, 1, "one");
    check(first.accepted == 2 && !first.backpressured && first.sequence == 1,
          "first reliable publication not admitted to both subscribers");
    check(bus.queued_payload_bytes() == 6,
          "reliable fanout byte ledger did not count both subscriber edges");
    auto blocked = bus.publish(a.group_id(), "reliable", 10, 1, "two");
    check(blocked.backpressured && blocked.accepted == 0 && blocked.sequence == 0,
          "reliable publication was partially admitted under backpressure");
    check(!bus.try_publish_reliable(a.group_id(), "reliable", 10, 1, "try-full"),
          "nonblocking RELIABLE try admitted a full subscriber edge");
    check(bus.queued_payload_bytes() == 6,
          "failed reliable admission changed byte accounting");
    check(bus.try_read(a.group_id(), "reliable", 11, 1).value() == "one",
          "subscriber 11 reliable value mismatch");
    check(bus.try_read(a.group_id(), "reliable", 12, 1).value() == "one",
          "subscriber 12 reliable value mismatch");
    check(bus.queued_payload_bytes() == 0,
          "reliable reads did not reclaim payload bytes");

    auto second_try =
        bus.try_publish_reliable(a.group_id(), "reliable", 10, 1, "two");
    check(second_try.has_value(),
          "nonblocking RELIABLE try did not admit reclaimed capacity");
    auto second = second_try.value();
    check(second.accepted == 2 && !second.backpressured && second.sequence == 2,
          "reliable capacity was not reclaimed");
    check(bus.queued_payload_bytes() == 6,
          "second reliable fanout byte ledger mismatch");

    check(memberships.leave(a.group_id(), 12, 1), "leave A for subscriber 12");
    check(bus.subscriber_count(a.group_id(), "reliable") == 1,
          "stale membership subscription was not pruned");
    check(bus.queued_payload_bytes() == 3,
          "stale subscription prune did not reclaim its queued payload bytes");
    check(memberships.contains(b.group_id(), 11, 1),
          "A membership mutation corrupted B");
    check(rejected([&] {
        (void)bus.publish(a.group_id(), "reliable", 12, 1, "stale");
    }), "departed member published using a cached group/topic name");
    check(rejected([&] {
        (void)bus.try_publish_reliable(a.group_id(), "reliable", 12, 1, "stale");
    }), "try publish accepted a departed actor generation");
    check(rejected([&] {
        (void)bus.try_read(a.group_id(), "reliable", 12, 1);
    }), "departed member read using a cached topic name");


    check(rejected([&] {
        (void)bus.try_publish_reliable(a.group_id(), "latest", 10, 1, "not-core");
    }), "try RELIABLE accidentally admitted a LATEST extension topic");
    check(bus.subscribe(a.group_id(), "latest", 11, 1), "latest subscribe");
    auto latest1 = bus.publish(a.group_id(), "latest", 10, 1, "old");
    auto latest2 = bus.publish(a.group_id(), "latest", 10, 1, "new");
    check(latest1.accepted == 1 && latest2.coalesced == 1
            && latest1.sequence == 1 && latest2.sequence == 2,
          "LATEST did not coalesce unread value");
    check(bus.queued_payload_bytes() == 6,
          "LATEST replacement leaked or double-counted payload bytes");
    check(bus.try_read(a.group_id(), "latest", 11, 1).value() == "new",
          "LATEST did not retain newest value");
    check(bus.queued_payload_bytes() == 3,
          "LATEST read did not reclaim replacement payload bytes");

    check(memberships.join(b, 10, 1), "publisher join B");
    // Even without recipients, accepted publications consume a per-topic
    // sequence. The separate bus does not alter primary test quotas.
    NativeActorPubSubAdmission<std::string> empty_bus(memberships, 1, 4);
    empty_bus.define_topic(
            b.group_id(), "no-replay",
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable, 1);
    const auto empty_first = empty_bus.publish(
            b.group_id(), "no-replay", 10, 1, "one");
    const auto empty_second = empty_bus.publish(
            b.group_id(), "no-replay", 10, 1, "two");
    check(empty_first.sequence == 1 && empty_first.accepted == 0
            && empty_second.sequence == 2 && empty_second.accepted == 0,
          "zero-subscriber publications must sequence without replay");
    check(bus.subscribe(b.group_id(), "lossy", 11, 1), "lossy subscribe");
    auto lossy1 = bus.publish(b.group_id(), "lossy", 10, 1, "one");
    auto lossy2 = bus.publish(b.group_id(), "lossy", 10, 1, "two");
    check(lossy1.accepted == 1 && lossy2.dropped == 1
            && lossy1.sequence == 1 && lossy2.sequence == 2,
          "LOSSY did not drop on full queue");
    check(bus.queued_payload_bytes() == 6,
          "LOSSY drop changed byte accounting");

    check(rejected([&] { (void)bus.publish(a.group_id(), "reliable", 13, 1, "bad"); }),
          "non-member publisher admitted");

    check(bus.close_group(a.group_id()) == 2,
          "group close did not remove both A topics");
    check(bus.queued_payload_bytes() == 3,
          "closing A did not reclaim exactly A's queued payload bytes");
    check(rejected([&] {
        (void)bus.publish(a.group_id(), "reliable", 10, 1, "after-close");
    }), "closed group topic remained publishable through cached name");
    check(bus.try_read(b.group_id(), "lossy", 11, 1).value() == "one",
          "closing A must not corrupt queued topic B delivery");
    check(bus.queued_payload_bytes() == 0,
          "reading surviving B payload did not reclaim graph bytes");
    check(bus.publish(b.group_id(), "lossy", 10, 1, "after-a-close").accepted == 1,
          "closing A must not revoke B publication");
    check(bus.try_read(b.group_id(), "lossy", 11, 1).value() == "after-a-close",
          "closing A must not revoke B subscription");
    bus.remove_actor(11, 1);
    check(bus.subscriber_count(b.group_id(), "lossy") == 0,
          "retiring actor must remove its surviving B subscription");
    check(bus.close_group(a.group_id()) == 0,
          "repeated group topic shutdown must be idempotent");
    check(bus.queued_payload_bytes() == 0,
          "all native pub-sub payload bytes must be reclaimed");

    /*
     * Tiny resource budgets prove byte and preallocated-slot admission without
     * requiring large allocations in the test process.
     */
    NativeActorPubSubAdmission<std::string> bounded(
        memberships,
        4,
        4,
        4,   // per-subscription payload bytes
        6,   // graph-wide logical queued bytes
        4,   // per-topic queue capacity
        3);  // total preallocated queue slots
    bounded.define_topic(
        b.group_id(), "bounded",
        NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable,
        1);
    check(bounded.subscribe(b.group_id(), "bounded", 11, 1),
          "bounded subscriber 11 admission failed");
    check(bounded.allocated_queue_slots() == 1,
          "bounded queue-slot ledger mismatch");
    check(rejected([&] {
        (void)bounded.publish(
            b.group_id(), "bounded", 10, 1, "12345");
    }), "oversized payload bypassed per-edge byte quota");

    check(bounded.publish(
              b.group_id(), "bounded", 10, 1, "abc").accepted == 1,
          "bounded payload was not admitted");
    check(bounded.queued_payload_bytes() == 3,
          "bounded payload byte ledger mismatch");
    check(bounded.try_read(
              b.group_id(), "bounded", 11, 1).value() == "abc",
          "bounded payload read mismatch");
    check(bounded.queued_payload_bytes() == 0,
          "bounded read did not reclaim bytes");

    bounded.define_topic(
        b.group_id(), "slot-heavy",
        NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable,
        2);
    check(bounded.subscribe(b.group_id(), "slot-heavy", 11, 1),
          "slot-heavy first subscription failed");
    check(bounded.allocated_queue_slots() == 3,
          "global queue-slot ledger did not include both topics");
    check(rejected([&] {
        (void)bounded.subscribe(b.group_id(), "slot-heavy", 10, 1);
    }), "global queue-slot budget admitted an unbounded empty subscription");

    check(bounded.close_group(b.group_id()) == 2,
          "bounded group close did not reclaim both topics");
    check(bounded.allocated_queue_slots() == 0,
          "group close leaked preallocated queue slots");
    check(bounded.queued_payload_bytes() == 0,
          "group close leaked bounded payload bytes");

    check(memberships.join(b, 12, 1),
          "subscriber 12 join B for graph-byte test");
    NativeActorPubSubAdmission<std::string> graph_bounded(
        memberships,
        2,
        4,
        4,   // per-edge bytes
        5,   // graph-wide bytes: smaller than 3 bytes * 2 subscribers
        2,
        4);
    graph_bounded.define_topic(
        b.group_id(), "graph",
        NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable,
        1);
    check(graph_bounded.subscribe(b.group_id(), "graph", 11, 1),
          "graph subscriber 11 admission failed");
    check(graph_bounded.subscribe(b.group_id(), "graph", 12, 1),
          "graph subscriber 12 admission failed");
    auto graph_blocked =
        graph_bounded.publish(b.group_id(), "graph", 10, 1, "abc");
    check(graph_blocked.backpressured && graph_blocked.accepted == 0,
          "graph byte quota allowed partial reliable fanout");
    check(graph_bounded.queued_payload_bytes() == 0,
          "graph-byte rejection changed payload ledger");
    check(graph_bounded.close_group(b.group_id()) == 1,
          "graph-byte test topic cleanup failed");
    check(graph_bounded.allocated_queue_slots() == 0,
          "graph-byte test leaked queue slots");

    /*
     * i64 and UTF8 lanes in one runtime share one physical resource ledger.
     * Payload type must not multiply per-actor subscription, queue-slot, or
     * graph-byte limits.
     */
    auto shared_resources = std::make_shared<NativeActorPubSubResourceLedger>(
        1, 9, 2);
    NativeActorPubSubAdmission<std::int64_t> shared_i64(
        memberships, 2, 1, 16, 9, 2, 2, shared_resources);
    NativeActorPubSubAdmission<std::string> shared_utf8(
        memberships, 2, 1, 16, 9, 2, 2, shared_resources);
    shared_i64.define_topic(
        b.group_id(), "shared-i64",
        NativeActorPubSubAdmission<std::int64_t>::DeliveryPolicy::Reliable,
        1);
    shared_utf8.define_topic(
        b.group_id(), "shared-utf8",
        NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable,
        1);
    check(shared_i64.subscribe(b.group_id(), "shared-i64", 11, 1),
          "shared i64 subscription failed");
    check(rejected([&] {
        (void)shared_utf8.subscribe(b.group_id(), "shared-utf8", 11, 1);
    }), "payload type multiplied shared per-actor subscription quota");
    check(shared_utf8.subscribe(b.group_id(), "shared-utf8", 12, 1),
          "shared UTF8 subscription failed");
    check(rejected([&] {
        (void)shared_utf8.subscribe(b.group_id(), "shared-utf8", 10, 1);
    }), "payload type multiplied shared preallocated-slot quota");
    check(shared_i64.allocated_queue_slots() == 2
              && shared_utf8.allocated_queue_slots() == 2,
          "typed lanes do not observe one shared slot ledger");

    auto shared_number =
        shared_i64.publish(b.group_id(), "shared-i64", 10, 1, INT64_C(7));
    check(shared_number.accepted == 1 && !shared_number.backpressured,
          "shared i64 publication was not admitted");
    check(shared_utf8.queued_payload_bytes() == sizeof(std::int64_t),
          "UTF8 lane did not observe i64 graph-byte reservation");
    auto shared_blocked =
        shared_utf8.publish(b.group_id(), "shared-utf8", 10, 1, "xy");
    check(shared_blocked.backpressured && shared_blocked.accepted == 0,
          "UTF8 lane bypassed graph bytes reserved by i64 lane");
    check(shared_utf8.queued_payload_bytes() == sizeof(std::int64_t),
          "failed cross-type admission changed shared byte ledger");

    check(shared_i64.try_read(
              b.group_id(), "shared-i64", 11, 1).value() == INT64_C(7),
          "shared i64 read mismatch");
    check(shared_utf8.queued_payload_bytes() == 0,
          "i64 read did not reclaim shared bytes for UTF8 lane");
    check(shared_utf8.publish(
              b.group_id(), "shared-utf8", 10, 1, "xy").accepted == 1,
          "UTF8 lane did not observe reclaimed shared bytes");
    check(shared_i64.queued_payload_bytes() == 2,
          "i64 lane did not observe UTF8 shared byte reservation");
    check(shared_utf8.try_read(
              b.group_id(), "shared-utf8", 12, 1).value() == "xy",
          "shared UTF8 read mismatch");
    check(shared_i64.queued_payload_bytes() == 0,
          "UTF8 read leaked shared graph bytes");

    check(shared_i64.unsubscribe(b.group_id(), "shared-i64", 11, 1),
          "shared i64 unsubscribe failed");
    check(shared_utf8.unsubscribe(b.group_id(), "shared-utf8", 12, 1),
          "shared UTF8 unsubscribe failed");
    check(shared_i64.allocated_queue_slots() == 0
              && shared_utf8.allocated_queue_slots() == 0,
          "cross-type teardown leaked shared queue slots");

    // The final cleanup of one actor incarnation must not race ahead of
    // re-registration. A concurrent registration has to wait for the
    // retirement callback while its membership lock remains held.
    NativeActorMembershipRegistry retire_gate(91, 2, 2);
    check(retire_gate.register_actor(901, 1), "register retirement fixture");
    const auto retire_group = retire_gate.create_group(1, 2);
    check(retire_gate.join(retire_group, 901, 1), "join retirement fixture");
    std::atomic<bool> cleaning{false};
    std::atomic<bool> release_cleanup{false};
    std::atomic<bool> began_register{false};
    std::atomic<bool> registered{false};
    std::atomic<bool> retired{false};
    std::thread old_actor([&] {
        retired.store(retire_gate.retire_actor_with_cleanup(901, 1, [&] {
            cleaning.store(true);
            while (!release_cleanup.load())
                std::this_thread::yield();
        }));
    });
    while (!cleaning.load()) std::this_thread::yield();
    std::thread next_actor([&] {
        began_register.store(true);
        registered.store(retire_gate.register_actor(901, 2));
    });
    while (!began_register.load()) std::this_thread::yield();
    for (int i = 0; i < 1000; ++i) std::this_thread::yield();
    const bool overtook_cleanup = registered.load();
    release_cleanup.store(true);
    old_actor.join();
    next_actor.join();
    check(!overtook_cleanup,
          "new actor registration overtook retirement cleanup");
    check(retired.load() && registered.load(),
          "actor retirement and replacement registration did not finish");
    check(!retire_gate.contains(1, 901, 1),
          "retired actor retained a prior-group membership");
}
