#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include "oreslang/runtime/NativeActorPubSub.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorPubSubAdmission;
using oreslang::runtime::NativeActorPubSubResourceLedger;

static void check(bool ok) { if (!ok) throw std::runtime_error("check"); }

template<class F>
static bool rejected(F&& f) {
    try { f(); } catch (const std::logic_error&) { return true; }
    return false;
}

int main() {
    NativeActorMembershipRegistry memberships(501, 2, 2);
    for (std::uint64_t id = 10; id <= 13; ++id)
        check(memberships.register_actor(id, 1));
    auto group = memberships.create_group(1, 8);
    for (std::uint64_t id = 10; id <= 13; ++id)
        check(memberships.join(group, id, 1));

    auto resources = std::make_shared<NativeActorPubSubResourceLedger>(
        1, 9, 2);
    NativeActorPubSubAdmission<std::int64_t> numbers(
        memberships, 2, 1, 16, 9, 2, 2, resources);
    NativeActorPubSubAdmission<std::string> text(
        memberships, 2, 1, 16, 9, 2, 2, resources);

    numbers.define_topic(
        1, "numbers",
        NativeActorPubSubAdmission<std::int64_t>::DeliveryPolicy::Reliable, 1);
    text.define_topic(
        1, "text",
        NativeActorPubSubAdmission<std::string>::DeliveryPolicy::Reliable, 1);

    check(numbers.subscribe(1, "numbers", 11, 1));
    check(rejected([&] { (void)text.subscribe(1, "text", 11, 1); }));
    check(text.subscribe(1, "text", 12, 1));
    check(rejected([&] { (void)text.subscribe(1, "text", 13, 1); }));
    check(numbers.allocated_queue_slots() == 2);
    check(text.allocated_queue_slots() == 2);

    auto first = numbers.publish(1, "numbers", 10, 1, INT64_C(7));
    check(first.accepted == 1 && !first.backpressured);
    check(numbers.queued_payload_bytes() == sizeof(std::int64_t));
    check(text.queued_payload_bytes() == sizeof(std::int64_t));

    auto blocked = text.publish(1, "text", 10, 1, std::string("xy"));
    check(blocked.backpressured && blocked.accepted == 0);
    check(text.queued_payload_bytes() == sizeof(std::int64_t));

    auto n = numbers.try_read(1, "numbers", 11, 1);
    check(n.has_value() && *n == INT64_C(7));
    check(text.queued_payload_bytes() == 0);

    auto admitted = text.publish(1, "text", 10, 1, std::string("xy"));
    check(admitted.accepted == 1 && !admitted.backpressured);
    check(numbers.queued_payload_bytes() == 2);
    auto s = text.try_read(1, "text", 12, 1);
    check(s.has_value() && *s == "xy");
    check(numbers.queued_payload_bytes() == 0);

    check(numbers.unsubscribe(1, "numbers", 11, 1));
    check(text.unsubscribe(1, "text", 12, 1));
    check(numbers.allocated_queue_slots() == 0);
    check(text.allocated_queue_slots() == 0);
    return 0;
}
