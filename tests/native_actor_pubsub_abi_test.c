#include "oreslang/runtime/NativeActorPubSubABI.h"

#include <stdint.h>
#include <stdlib.h>

static void check(int condition) {
    if (!condition) abort();
}

static OresSourceActorIdentity actor(
        uint64_t id,
        uint64_t domain,
        uint64_t slot,
        uint64_t generation) {
    OresSourceActorIdentity result = {
        .logical_id = id,
        .scheduler_domain = domain,
        .actor_slot = slot,
        .scheduler_generation = generation
    };
    return result;
}

int main(void) {
    OresNativeActorPubSubRuntime* runtime =
        ores_native_actor_pubsub_open(77, 8, 4, 8, 8);
    check(runtime != NULL);

    const OresSourceActorIdentity publisher = actor(101, 77, 1, 5);
    const OresSourceActorIdentity subscriber = actor(102, 77, 2, 8);
    const OresSourceActorIdentity foreign = actor(103, 88, 3, 1);

    check(ores_native_actor_pubsub_register_source_actor(
        runtime, publisher) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_register_source_actor(
        runtime, subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_register_source_actor(
        runtime, foreign) == ORES_NATIVE_PUBSUB_INVALID);

    check(ores_native_actor_pubsub_create_group(
        runtime, 1, 8) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_create_group(
        runtime, 2, 8) == ORES_NATIVE_PUBSUB_OK);

    check(ores_native_actor_pubsub_join(
        runtime, 1, publisher) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(
        runtime, 2, publisher) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(
        runtime, 1, subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(
        runtime, 2, subscriber) == ORES_NATIVE_PUBSUB_OK);

    check(ores_native_actor_pubsub_define_i64_topic(
        runtime, 1, "events",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_i64_topic(
        runtime, 2, "events",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_OK);

    check(ores_native_actor_pubsub_subscribe_i64(
        runtime, 1, "events", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_i64(
        runtime, 2, "events", subscriber) == ORES_NATIVE_PUBSUB_OK);

    OresNativePubSubPublishReceipt receipt = {0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 1, "events", publisher, 11, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 1 && receipt.accepted == 1
        && !receipt.backpressured);

    int64_t value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "events", subscriber, &value)
        == ORES_NATIVE_PUBSUB_OK);
    check(value == 11);

    /* Leaving A must remove only A's subscription. */
    check(ores_native_actor_pubsub_leave(
        runtime, 1, subscriber) == ORES_NATIVE_PUBSUB_OK);
    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 1, "events", publisher, 12, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 0 && receipt.accepted == 0);

    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 2, "events", publisher, 21, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 1 && receipt.accepted == 1);

    /* Capacity one produces bounded RELIABLE backpressure, never partial send. */
    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 2, "events", publisher, 22, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 1 && receipt.accepted == 0
        && receipt.backpressured);

    value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 2, "events", subscriber, &value)
        == ORES_NATIVE_PUBSUB_OK);
    check(value == 21);

    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 2, "events", subscriber, &value)
        == ORES_NATIVE_PUBSUB_EMPTY);

    check(ores_native_actor_pubsub_retire_source_actor(
        runtime, subscriber) == ORES_NATIVE_PUBSUB_OK);
    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 2, "events", publisher, 23, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 0 && receipt.accepted == 0);

    check(ores_native_actor_pubsub_close_group(
        runtime, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_close_group(
        runtime, 2) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_retire_source_actor(
        runtime, publisher) == ORES_NATIVE_PUBSUB_OK);

    ores_native_actor_pubsub_close(runtime);
    return 0;
}
