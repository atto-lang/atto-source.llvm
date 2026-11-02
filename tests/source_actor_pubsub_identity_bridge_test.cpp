#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include "oreslang/runtime/NativeActorPubSub.hpp"
#include "oreslang/runtime/SourceActorBootstrap.h"

#include <cstdint>
#include <cstdlib>
#include <sched.h>
#include <stdexcept>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorPubSubAdmission;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void actor_init(
        OresActorI64Heap* heap,
        std::uint64_t domain,
        std::uint64_t epoch) {
    (void)heap;
    if (domain == 0 || epoch == 0) std::abort();
}

static std::int64_t actor_message(
        OresActorI64Heap* heap,
        std::uint64_t domain,
        std::uint64_t epoch,
        std::int64_t payload) {
    (void)heap;
    if (domain == 0 || epoch == 0) std::abort();
    return payload;
}

static void await_ready(void* actor) {
    for (;;) {
        const int status = oreslang_source_actor_ready_status(actor);
        if (status == ORES_SOURCE_ACTOR_STARTUP_READY) return;
        check(status == ORES_SOURCE_ACTOR_STARTUP_PENDING
                || status == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING,
              "source actor failed before readiness");
        sched_yield();
    }
}

int main() {
    void* publisher = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    void* subscriber = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(publisher != nullptr && subscriber != nullptr,
          "source actor spawn failed");

    await_ready(publisher);
    await_ready(subscriber);

    OresSourceActorIdentity publisher_id{};
    OresSourceActorIdentity subscriber_id{};
    check(oreslang_source_actor_identity(publisher, &publisher_id),
          "publisher identity unavailable");
    check(oreslang_source_actor_identity(subscriber, &subscriber_id),
          "subscriber identity unavailable");

    check(publisher_id.logical_id != subscriber_id.logical_id,
          "distinct source actors reused logical identity");
    check(publisher_id.scheduler_domain == subscriber_id.scheduler_domain,
          "same source actor VM produced different scheduler domains");
    check(publisher_id.actor_slot != subscriber_id.actor_slot,
          "distinct source actors reused live scheduler slot");
    check(publisher_id.scheduler_generation != 0
            && subscriber_id.scheduler_generation != 0,
          "source actor generation fence missing");

    NativeActorMembershipRegistry memberships(
        publisher_id.scheduler_domain, 8, 4);
    check(memberships.register_actor(
              publisher_id.logical_id,
              publisher_id.scheduler_generation),
          "publisher registration failed");
    check(memberships.register_actor(
              subscriber_id.logical_id,
              subscriber_id.scheduler_generation),
          "subscriber registration failed");

    auto group = memberships.create_group(1, 8);
    check(memberships.join(
              group,
              publisher_id.logical_id,
              publisher_id.scheduler_generation),
          "publisher group join failed");
    check(memberships.join(
              group,
              subscriber_id.logical_id,
              subscriber_id.scheduler_generation),
          "subscriber group join failed");

    NativeActorPubSubAdmission<std::int64_t> bus(memberships, 4, 4);
    bus.define_topic(
        group.group_id(),
        "source-identity",
        NativeActorPubSubAdmission<std::int64_t>::DeliveryPolicy::Reliable,
        2);
    check(bus.subscribe(
              group.group_id(),
              "source-identity",
              subscriber_id.logical_id,
              subscriber_id.scheduler_generation),
          "source subscriber admission failed");

    const auto receipt = bus.publish(
        group.group_id(),
        "source-identity",
        publisher_id.logical_id,
        publisher_id.scheduler_generation,
        INT64_C(42));
    check(receipt.accepted == 1 && !receipt.backpressured,
          "source identity publication was not admitted");
    const auto value = bus.try_read(
        group.group_id(),
        "source-identity",
        subscriber_id.logical_id,
        subscriber_id.scheduler_generation);
    check(value.has_value() && value.value() == INT64_C(42),
          "source identity subscriber read mismatch");

    /*
     * Physical source actor retirement invalidates the opaque identity first;
     * control-plane retirement then removes the corresponding membership and
     * topic subscription. No replacement actor can inherit either by slot.
     */
    oreslang_source_actor_request_stop(subscriber);
    oreslang_source_actor_join_done(subscriber);
    OresSourceActorIdentity retired{};
    check(!oreslang_source_actor_identity(subscriber, &retired),
          "retired source actor still exposed identity");
    check(memberships.retire_actor(
              subscriber_id.logical_id,
              subscriber_id.scheduler_generation),
          "membership retirement failed");
    bus.remove_actor(
        subscriber_id.logical_id,
        subscriber_id.scheduler_generation);
    check(bus.subscriber_count(
              group.group_id(), "source-identity") == 0,
          "retired source actor subscription leaked");

    const auto empty = bus.publish(
        group.group_id(),
        "source-identity",
        publisher_id.logical_id,
        publisher_id.scheduler_generation,
        INT64_C(99));
    check(empty.subscribers == 0 && empty.accepted == 0,
          "publication targeted physically retired source actor");

    oreslang_source_actor_request_stop(publisher);
    oreslang_source_actor_join_done(publisher);
    check(memberships.retire_actor(
              publisher_id.logical_id,
              publisher_id.scheduler_generation),
          "publisher membership retirement failed");
    bus.remove_actor(
        publisher_id.logical_id,
        publisher_id.scheduler_generation);
    return 0;
}
