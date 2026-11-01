#include "oreslang/runtime/NativeActorPubSub.hpp"

#include <stdexcept>
#include <string>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorPubSubAdmission;

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
    check(first.accepted == 2 && !first.backpressured,
          "first reliable publication not admitted to both subscribers");
    auto blocked = bus.publish(a.group_id(), "reliable", 10, 1, "two");
    check(blocked.backpressured && blocked.accepted == 0,
          "reliable publication was partially admitted under backpressure");
    check(bus.try_read(a.group_id(), "reliable", 11, 1).value() == "one",
          "subscriber 11 reliable value mismatch");
    check(bus.try_read(a.group_id(), "reliable", 12, 1).value() == "one",
          "subscriber 12 reliable value mismatch");

    auto second = bus.publish(a.group_id(), "reliable", 10, 1, "two");
    check(second.accepted == 2 && !second.backpressured,
          "reliable capacity was not reclaimed");

    check(memberships.leave(a.group_id(), 12, 1), "leave A for subscriber 12");
    check(bus.subscriber_count(a.group_id(), "reliable") == 1,
          "stale membership subscription was not pruned");
    check(memberships.contains(b.group_id(), 11, 1),
          "A membership mutation corrupted B");

    check(bus.subscribe(a.group_id(), "latest", 11, 1), "latest subscribe");
    auto latest1 = bus.publish(a.group_id(), "latest", 10, 1, "old");
    auto latest2 = bus.publish(a.group_id(), "latest", 10, 1, "new");
    check(latest1.accepted == 1 && latest2.coalesced == 1,
          "LATEST did not coalesce unread value");
    check(bus.try_read(a.group_id(), "latest", 11, 1).value() == "new",
          "LATEST did not retain newest value");

    check(memberships.join(b, 10, 1), "publisher join B");
    check(bus.subscribe(b.group_id(), "lossy", 11, 1), "lossy subscribe");
    auto lossy1 = bus.publish(b.group_id(), "lossy", 10, 1, "one");
    auto lossy2 = bus.publish(b.group_id(), "lossy", 10, 1, "two");
    check(lossy1.accepted == 1 && lossy2.dropped == 1,
          "LOSSY did not drop on full queue");

    check(rejected([&] { (void)bus.publish(a.group_id(), "reliable", 13, 1, "bad"); }),
          "non-member publisher admitted");

    bus.remove_actor(11, 1);
    check(bus.subscriber_count(a.group_id(), "reliable") == 0,
          "actor pub-sub cleanup leaked reliable subscription");
    check(bus.close_group(a.group_id()) == 2,
          "group close did not remove both A topics");
}
