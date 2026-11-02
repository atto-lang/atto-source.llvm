#include "oreslang/runtime/NativeActorPubSubABI.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

    /* String topics share the same actor incarnation/membership fence. */
    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 2, "strings",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 2, "newest",
        ORES_NATIVE_PUBSUB_LATEST, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 2, "losses",
        ORES_NATIVE_PUBSUB_LOSSY, 1) == ORES_NATIVE_PUBSUB_OK);
    /* A topic name is single-typed, regardless of the native bus lane. */
    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 1, "events",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_define_i64_topic(
        runtime, 2, "strings",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_subscribe_utf8(
        runtime, 2, "strings", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_utf8(
        runtime, 2, "newest", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_utf8(
        runtime, 2, "losses", subscriber) == ORES_NATIVE_PUBSUB_OK);

    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "strings", publisher, "hello", 5, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.accepted == 1 && !receipt.backpressured);
    receipt = (OresNativePubSubPublishReceipt){0};
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "strings", publisher, "blocked", 7, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.accepted == 0 && receipt.backpressured);

    char small[3] = {0};
    size_t length = 0;
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "strings", subscriber, small, sizeof small, &length)
        == ORES_NATIVE_PUBSUB_RESOURCE_EXHAUSTED);
    check(length == 5);
    char received[16] = {0};
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "strings", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_OK);
    check(length == 5 && memcmp(received, "hello", 5) == 0);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "strings", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_EMPTY);

    /* UTF-8 validity is checked before mutation; reject overlong encodings. */
    const char invalid_utf8[] = {(char)0xc0, (char)0xaf};
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "strings", publisher, invalid_utf8, 2, &receipt)
        == ORES_NATIVE_PUBSUB_INVALID);
    const char multilingual[] = "h\xc3\xa9llo";
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "strings", publisher,
        multilingual, sizeof multilingual - 1, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "strings", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_OK);
    check(length == sizeof multilingual - 1
        && memcmp(received, multilingual, length) == 0);

    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "newest", publisher, "old", 3, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "newest", publisher, "new", 3, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.coalesced == 1);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "newest", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_OK);
    check(length == 3 && memcmp(received, "new", 3) == 0);

    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "losses", publisher, "first", 5, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "losses", publisher, "drop", 4, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.dropped == 1);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "losses", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_OK);
    check(length == 5 && memcmp(received, "first", 5) == 0);

    /* Leaving A must remove only A's subscription. */
    check(ores_native_actor_pubsub_leave(
        runtime, 1, subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_unsubscribe_i64(
        runtime, 1, "events", subscriber)
        == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "events", subscriber, &value)
        == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_join(
        runtime, 1, subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_i64(
        runtime, 1, "events", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 1, "events", publisher, 101, &receipt)
        == ORES_NATIVE_PUBSUB_OK);
    check(receipt.subscribers == 1 && receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "events", subscriber, &value)
        == ORES_NATIVE_PUBSUB_OK);
    check(value == 101);
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
    check(ores_native_actor_pubsub_unsubscribe_i64(
        runtime, 2, "events", subscriber) == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_subscribe_utf8(
        runtime, 2, "strings", subscriber) == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_publish_utf8(
        runtime, 2, "strings", subscriber, "stale", 5, &receipt)
        == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "strings", subscriber, received, sizeof received, &length)
        == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
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

    /* i64 and UTF-8 share one per-actor subscription credit. */
    OresNativeActorPubSubRuntime* quota = ores_native_actor_pubsub_open(99, 2, 2, 4, 1);
    check(quota != NULL);
    const OresSourceActorIdentity one = actor(201, 99, 1, 1);
    check(ores_native_actor_pubsub_register_source_actor(quota, one) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_create_group(quota, 1, 2) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(quota, 1, one) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_i64_topic(
        quota, 1, "ints", ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_utf8_topic(
        quota, 1, "texts", ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_i64(
        quota, 1, "ints", one) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_utf8(
        quota, 1, "texts", one) == ORES_NATIVE_PUBSUB_DENIED_OR_STALE);
    check(ores_native_actor_pubsub_unsubscribe_i64(
        quota, 1, "ints", one) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_utf8(
        quota, 1, "texts", one) == ORES_NATIVE_PUBSUB_OK);
    ores_native_actor_pubsub_close(quota);
    return 0;
}
