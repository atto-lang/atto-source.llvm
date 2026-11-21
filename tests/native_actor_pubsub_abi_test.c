#include "oreslang/runtime/NativeActorPubSubABI.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Distinct V2 receipt symbols must not change the footprint of callers
 * compiled against the frozen V1 struct. */
_Static_assert(offsetof(OresNativePubSubPublishReceiptV2, sequence)
                   >= sizeof(OresNativePubSubPublishReceipt),
               "V2 receipt sequence overlaps the legacy V1 footprint");

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

    /* A C1 Unicode control is legal UTF-8 but not a legal topic name.
     * .NET rejects U+0085 through char.IsControl; native lanes must agree. */
    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 1, "bad-\xC2\x85-name",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_INVALID);
    check(ores_native_actor_pubsub_define_i64_topic(
        runtime, 1, "bad-\xC2\x9F-name",
        ORES_NATIVE_PUBSUB_RELIABLE, 1) == ORES_NATIVE_PUBSUB_INVALID);
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

    /* V2 receipts are additive ABI symbols; V1 struct stays byte-for-byte.
     * RELIABLE retry after a full queue must not consume a topic sequence. */
    check(ores_native_actor_pubsub_define_i64_topic(
        runtime, 1, "sequence-v2", ORES_NATIVE_PUBSUB_RELIABLE, 1)
        == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_i64(
        runtime, 1, "sequence-v2", subscriber) == ORES_NATIVE_PUBSUB_OK);
    OresNativePubSubPublishReceiptV2 v2 = {0};
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 31, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 1 && v2.subscribers == 1
        && v2.accepted == 1 && !v2.backpressured);
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 32, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 0 && v2.accepted == 0 && v2.backpressured);
    int64_t sequence_value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "sequence-v2", subscriber, &sequence_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(sequence_value == 31);
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 33, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 2 && v2.accepted == 1);
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "sequence-v2", subscriber, &sequence_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(sequence_value == 33);

    /* No subscribers still consumes a successful topic sequence, and the
     * legacy V1 symbol must advance the SAME counter as the V2 symbol. */
    check(ores_native_actor_pubsub_unsubscribe_i64(
        runtime, 1, "sequence-v2", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 34, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 3 && v2.subscribers == 0 && v2.accepted == 0);
    OresNativePubSubPublishReceipt legacy_sequence_receipt = {0};
    check(ores_native_actor_pubsub_publish_i64(
        runtime, 1, "sequence-v2", publisher, 35,
        &legacy_sequence_receipt) == ORES_NATIVE_PUBSUB_OK);
    check(legacy_sequence_receipt.subscribers == 0
        && legacy_sequence_receipt.accepted == 0);
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 36, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 5 && v2.subscribers == 0 && v2.accepted == 0);

    check(ores_native_actor_pubsub_define_utf8_topic(
        runtime, 2, "utf8-sequence-v2", ORES_NATIVE_PUBSUB_RELIABLE, 1)
        == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_subscribe_utf8(
        runtime, 2, "utf8-sequence-v2", subscriber) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_publish_utf8_v2(
        runtime, 2, "utf8-sequence-v2", publisher, "one", 3, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 1 && v2.accepted == 1);
    check(ores_native_actor_pubsub_publish_utf8_v2(
        runtime, 2, "utf8-sequence-v2", publisher, "two", 3, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 0 && v2.backpressured && v2.accepted == 0);
    char v2_buffer[8] = {0};
    size_t v2_length = 0;
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "utf8-sequence-v2", subscriber,
        v2_buffer, sizeof v2_buffer, &v2_length) == ORES_NATIVE_PUBSUB_OK);
    check(v2_length == 3 && memcmp(v2_buffer, "one", 3) == 0);
    check(ores_native_actor_pubsub_publish_utf8_v2(
        runtime, 2, "utf8-sequence-v2", publisher, "two", 3, &v2)
        == ORES_NATIVE_PUBSUB_OK);
    check(v2.sequence == 2 && v2.accepted == 1);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "utf8-sequence-v2", subscriber,
        v2_buffer, sizeof v2_buffer, &v2_length) == ORES_NATIVE_PUBSUB_OK);
    check(v2_length == 3 && memcmp(v2_buffer, "two", 3) == 0);

    /* Guest-facing typed Result admission uses the SAME live membership
     * and queue reservation path, but refuses non-RELIABLE policy. */
    check(ores_native_actor_pubsub_subscribe_i64(
        runtime, 1, "sequence-v2", subscriber) == ORES_NATIVE_PUBSUB_OK);
    OresNativePubSubTryPublishResultV1 result = {0};
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        runtime, 1, "sequence-v2", publisher, 41, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && result.receipt.sequence == 6 && result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        runtime, 1, "sequence-v2", publisher, 42, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_FULL
        && result.receipt.sequence == 0 && result.receipt.accepted == 0);
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "sequence-v2", subscriber, &sequence_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(sequence_value == 41);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        runtime, 1, "sequence-v2", publisher, 43, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && result.receipt.sequence == 7 && result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_read_i64(
        runtime, 1, "sequence-v2", subscriber, &sequence_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(sequence_value == 43);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        runtime, 1, "sequence-v2", publisher, 44, NULL)
        == ORES_NATIVE_PUBSUB_INVALID);
    result.tag = ORES_NATIVE_PUBSUB_TRY_FULL;
    result.receipt.sequence = UINT64_C(0xdeadbeef);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        runtime, 1, "sequence-v2", foreign, 45, &result)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_INVALID
        && result.receipt.sequence == 0);
    check(ores_native_actor_pubsub_unsubscribe_i64(
        runtime, 1, "sequence-v2", subscriber) == ORES_NATIVE_PUBSUB_OK);

    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "utf8-sequence-v2", publisher, "three", 5, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && result.receipt.sequence == 3 && result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "utf8-sequence-v2", publisher, "four", 4, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_FULL
        && result.receipt.sequence == 0 && result.receipt.accepted == 0);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "utf8-sequence-v2", subscriber,
        v2_buffer, sizeof v2_buffer, &v2_length) == ORES_NATIVE_PUBSUB_OK);
    check(v2_length == 5 && memcmp(v2_buffer, "three", 5) == 0);
    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "utf8-sequence-v2", publisher, "four", 4, &result)
        == ORES_NATIVE_PUBSUB_OK);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && result.receipt.sequence == 4 && result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_read_utf8(
        runtime, 2, "utf8-sequence-v2", subscriber,
        v2_buffer, sizeof v2_buffer, &v2_length) == ORES_NATIVE_PUBSUB_OK);
    check(v2_length == 4 && memcmp(v2_buffer, "four", 4) == 0);
    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "utf8-sequence-v2", foreign, "foreign", 7, &result)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_INVALID
        && result.receipt.sequence == 0);

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


    /* Invalid publisher identities and NULL result pointers fail closed
     * without reading an invalid actor's source heap or touching receipts. */
    v2.sequence = UINT64_C(0xdeadbeef);
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", foreign, 34, &v2)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(v2.sequence == UINT64_C(0xdeadbeef));
    check(ores_native_actor_pubsub_publish_i64_v2(
        runtime, 1, "sequence-v2", publisher, 34, NULL)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(ores_native_actor_pubsub_publish_utf8_v2(
        runtime, 2, "utf8-sequence-v2", foreign, "bad", 3, &v2)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(v2.sequence == UINT64_C(0xdeadbeef));
    check(ores_native_actor_pubsub_publish_utf8_v2(
        runtime, 2, "utf8-sequence-v2", publisher, "bad", 3, NULL)
        == ORES_NATIVE_PUBSUB_INVALID);

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
    /* A typed try_publish is RELIABLE only; no partial guest Result from
     * LATEST/LOSSY and no topic sequence consumed on policy denial. */
    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "newest", publisher, "nope", 4, &result)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_INVALID
        && result.receipt.sequence == 0);
    check(ores_native_actor_pubsub_try_publish_utf8_result_v1(
        runtime, 2, "losses", publisher, "nope", 4, &result)
        == ORES_NATIVE_PUBSUB_INVALID);
    check(result.tag == ORES_NATIVE_PUBSUB_TRY_INVALID
        && result.receipt.sequence == 0);
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

    /*
     * Cross-backend strict admission: mirrors Java ActorEventBus
     * tryPublishReliable, which now permits only immediate queue admission,
     * not the separate asynchronous publish() pending-writer budget.
     * No recipients still constitutes a committed topic sequence.
     */
    OresNativeActorPubSubRuntime* parity =
        ores_native_actor_pubsub_open(111, 2, 2, 2, 2);
    check(parity != NULL);
    const OresSourceActorIdentity parity_pub = actor(401, 111, 1, 1);
    const OresSourceActorIdentity parity_sub = actor(402, 111, 2, 1);
    check(ores_native_actor_pubsub_register_source_actor(
        parity, parity_pub) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_register_source_actor(
        parity, parity_sub) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_create_group(
        parity, 1, 2) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(
        parity, 1, parity_pub) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_join(
        parity, 1, parity_sub) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_define_i64_topic(
        parity, 1, "strict", ORES_NATIVE_PUBSUB_RELIABLE, 1)
        == ORES_NATIVE_PUBSUB_OK);

    OresNativePubSubTryPublishResultV1 strict_result = {0};
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        parity, 1, "strict", parity_pub, 10, &strict_result)
        == ORES_NATIVE_PUBSUB_OK);
    check(strict_result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && strict_result.receipt.sequence == 1
        && strict_result.receipt.subscribers == 0
        && strict_result.receipt.accepted == 0);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        parity, 1, "strict", parity_pub, 11, &strict_result)
        == ORES_NATIVE_PUBSUB_OK);
    check(strict_result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && strict_result.receipt.sequence == 2
        && strict_result.receipt.accepted == 0);

    check(ores_native_actor_pubsub_subscribe_i64(
        parity, 1, "strict", parity_sub) == ORES_NATIVE_PUBSUB_OK);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        parity, 1, "strict", parity_pub, 12, &strict_result)
        == ORES_NATIVE_PUBSUB_OK);
    check(strict_result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && strict_result.receipt.sequence == 3
        && strict_result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        parity, 1, "strict", parity_pub, 13, &strict_result)
        == ORES_NATIVE_PUBSUB_OK);
    check(strict_result.tag == ORES_NATIVE_PUBSUB_TRY_FULL
        && strict_result.receipt.sequence == 0
        && strict_result.receipt.accepted == 0);
    int64_t parity_value = -1;
    check(ores_native_actor_pubsub_try_read_i64(
        parity, 1, "strict", parity_sub, &parity_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(parity_value == 12);
    check(ores_native_actor_pubsub_try_publish_i64_result_v1(
        parity, 1, "strict", parity_pub, 14, &strict_result)
        == ORES_NATIVE_PUBSUB_OK);
    check(strict_result.tag == ORES_NATIVE_PUBSUB_TRY_OK
        && strict_result.receipt.sequence == 4
        && strict_result.receipt.accepted == 1);
    check(ores_native_actor_pubsub_try_read_i64(
        parity, 1, "strict", parity_sub, &parity_value)
        == ORES_NATIVE_PUBSUB_OK);
    check(parity_value == 14);
    ores_native_actor_pubsub_close(parity);
    return 0;
}
