#include "oreslang/runtime/NativeActorPubSubABI.h"
#include "oreslang/runtime/SourceActorBootstrap.h"

#include <cstdint>
#include <cstdlib>
#include <sched.h>
#include <stdexcept>

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

    OresNativeActorPubSubRuntime* pubsub =
        ores_native_actor_pubsub_open(
            publisher_id.scheduler_domain, 8, 4, 4, 4);
    check(pubsub != nullptr, "native pub-sub ABI open failed");

    check(ores_native_actor_pubsub_register_source_actor(
              pubsub, publisher_id) == ORES_NATIVE_PUBSUB_OK,
          "publisher registration failed");
    check(ores_native_actor_pubsub_register_source_actor(
              pubsub, subscriber_id) == ORES_NATIVE_PUBSUB_OK,
          "subscriber registration failed");
    check(ores_native_actor_pubsub_create_group(
              pubsub, 1, 8) == ORES_NATIVE_PUBSUB_OK,
          "group creation failed");
    check(ores_native_actor_pubsub_join(
              pubsub, 1, publisher_id) == ORES_NATIVE_PUBSUB_OK,
          "publisher group join failed");
    check(ores_native_actor_pubsub_join(
              pubsub, 1, subscriber_id) == ORES_NATIVE_PUBSUB_OK,
          "subscriber group join failed");
    check(ores_native_actor_pubsub_define_i64_topic(
              pubsub, 1, "source-identity",
              ORES_NATIVE_PUBSUB_RELIABLE, 2)
              == ORES_NATIVE_PUBSUB_OK,
          "source identity topic definition failed");
    check(ores_native_actor_pubsub_subscribe_i64(
              pubsub, 1, "source-identity", subscriber_id)
              == ORES_NATIVE_PUBSUB_OK,
          "source subscriber admission failed");

    OresNativePubSubPublishReceipt receipt{};
    check(ores_native_actor_pubsub_publish_i64(
              pubsub, 1, "source-identity",
              publisher_id, INT64_C(42), &receipt)
              == ORES_NATIVE_PUBSUB_OK,
          "source identity publication failed");
    check(receipt.accepted == 1 && !receipt.backpressured,
          "source identity publication was not admitted");

    std::int64_t value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
              pubsub, 1, "source-identity",
              subscriber_id, &value)
              == ORES_NATIVE_PUBSUB_OK,
          "source identity subscriber read failed");
    check(value == INT64_C(42),
          "source identity subscriber read mismatch");

    /*
     * Physical source actor retirement invalidates the opaque identity.
     * The saved generation-fenced identity then authorizes only trusted
     * runtime cleanup; no stale pointer is needed to reclaim subscription
     * state.
     */
    oreslang_source_actor_request_stop(subscriber);
    oreslang_source_actor_join_done(subscriber);
    OresSourceActorIdentity retired{};
    check(!oreslang_source_actor_identity(subscriber, &retired),
          "retired source actor still exposed identity");
    check(ores_native_actor_pubsub_retire_source_actor(
              pubsub, subscriber_id) == ORES_NATIVE_PUBSUB_OK,
          "pub-sub subscriber retirement failed");

    receipt = OresNativePubSubPublishReceipt{};
    check(ores_native_actor_pubsub_publish_i64(
              pubsub, 1, "source-identity",
              publisher_id, INT64_C(99), &receipt)
              == ORES_NATIVE_PUBSUB_OK,
          "post-retirement publication failed unexpectedly");
    check(receipt.subscribers == 0 && receipt.accepted == 0,
          "publication targeted physically retired source actor");

    oreslang_source_actor_request_stop(publisher);
    oreslang_source_actor_join_done(publisher);
    check(ores_native_actor_pubsub_retire_source_actor(
              pubsub, publisher_id) == ORES_NATIVE_PUBSUB_OK,
          "pub-sub publisher retirement failed");
    check(ores_native_actor_pubsub_close_group(
              pubsub, 1) == ORES_NATIVE_PUBSUB_OK,
          "group close failed");
    ores_native_actor_pubsub_close(pubsub);
    return 0;
}
