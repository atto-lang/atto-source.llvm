#pragma once

#include <stddef.h>
#include <stdint.h>

#include "oreslang/runtime/SourceActorBootstrap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OresNativeActorPubSubRuntime OresNativeActorPubSubRuntime;

typedef enum {
    ORES_NATIVE_PUBSUB_OK = 0,
    ORES_NATIVE_PUBSUB_INVALID = 1,
    ORES_NATIVE_PUBSUB_DENIED_OR_STALE = 2,
    ORES_NATIVE_PUBSUB_RESOURCE_EXHAUSTED = 3,
    ORES_NATIVE_PUBSUB_NOT_FOUND_OR_CLOSED = 4,
    ORES_NATIVE_PUBSUB_EMPTY = 5,
    ORES_NATIVE_PUBSUB_INTERNAL_ERROR = 6
} OresNativePubSubStatus;

typedef enum {
    ORES_NATIVE_PUBSUB_RELIABLE = 1,
    ORES_NATIVE_PUBSUB_LATEST = 2,
    ORES_NATIVE_PUBSUB_LOSSY = 3
} OresNativePubSubPolicy;

typedef struct {
    uint64_t subscribers;
    uint64_t accepted;
    uint64_t dropped;
    uint64_t coalesced;
    int backpressured;
} OresNativePubSubPublishReceipt;

/*
 * Trusted native runtime control ABI. These handles/functions are targets for
 * compiler-generated lowering and host/runtime tests; they are not reflection
 * or a guest FFI surface. Every function catches C++ exceptions internally.
 */
OresNativeActorPubSubRuntime* ores_native_actor_pubsub_open(
    uint64_t runtime_domain,
    size_t max_groups,
    size_t max_groups_per_actor,
    size_t max_topics,
    size_t max_subscriptions_per_actor);

void ores_native_actor_pubsub_close(OresNativeActorPubSubRuntime* runtime);

OresNativePubSubStatus ores_native_actor_pubsub_register_source_actor(
    OresNativeActorPubSubRuntime* runtime,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_retire_source_actor(
    OresNativeActorPubSubRuntime* runtime,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_create_group(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    size_t max_members);

OresNativePubSubStatus ores_native_actor_pubsub_close_group(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id);

OresNativePubSubStatus ores_native_actor_pubsub_join(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_leave(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_define_i64_topic(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresNativePubSubPolicy policy,
    size_t capacity);

OresNativePubSubStatus ores_native_actor_pubsub_subscribe_i64(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_unsubscribe_i64(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity actor);

OresNativePubSubStatus ores_native_actor_pubsub_publish_i64(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity publisher,
    int64_t value,
    OresNativePubSubPublishReceipt* out_receipt);

OresNativePubSubStatus ores_native_actor_pubsub_try_read_i64(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity subscriber,
    int64_t* out_value);

/*
 * UTF-8 String source payload bridge. Only trusted compiler/runtime code may
 * call these C symbols. payload_length is explicit (embedded NUL is allowed),
 * and all names are validated as bounded NUL-terminated UTF-8 strings.
 * A read with insufficient capacity returns RESOURCE_EXHAUSTED, writes the
 * required byte count to out_length and leaves the queued event untouched.
 * No guest-visible raw string pointer or reconstructed actor authority.
 */
OresNativePubSubStatus ores_native_actor_pubsub_define_utf8_topic(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresNativePubSubPolicy policy,
    size_t capacity);

OresNativePubSubStatus ores_native_actor_pubsub_subscribe_utf8(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity subscriber);

OresNativePubSubStatus ores_native_actor_pubsub_unsubscribe_utf8(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity subscriber);

OresNativePubSubStatus ores_native_actor_pubsub_publish_utf8(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity publisher,
    const char* payload,
    size_t payload_length,
    OresNativePubSubPublishReceipt* out_receipt);

OresNativePubSubStatus ores_native_actor_pubsub_try_read_utf8(
    OresNativeActorPubSubRuntime* runtime,
    uint64_t group_id,
    const char* topic_name,
    OresSourceActorIdentity subscriber,
    char* out_buffer,
    size_t out_capacity,
    size_t* out_length);

#ifdef __cplusplus
}
#endif
