#include "oreslang/runtime/NativeActorPubSubABI.h"
#include "oreslang/runtime/SourceActorBootstrap.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sched.h>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Only the native integration harness owns this host-side runtime pointer.
// Source actors cannot create or pass unverified publisher identities.
static std::atomic<OresNativeActorPubSubRuntime*> live_pubsub{nullptr};
static std::atomic<std::uint64_t> publisher_turn_id{0};
static std::atomic<std::uint64_t> publisher_turn_generation{0};
static std::atomic<std::uint64_t> subscriber_turn_id{0};
static std::atomic<std::uint64_t> subscriber_turn_generation{0};
static std::atomic<int> turn_publish_status{-1};
static std::atomic<int> turn_publish_tag{-1};
static std::atomic<std::uint64_t> turn_publish_sequence{0};

static void require_probe(
        const std::atomic<std::uint64_t>& value, const char* message) {
    const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
    while (value.load(std::memory_order_acquire) == 0
            && std::chrono::steady_clock::now() < deadline) {
        sched_yield();
    }
    check(value.load(std::memory_order_acquire) != 0, message);
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
    if (payload == INT64_C(701) || payload == INT64_C(702)
            || payload == INT64_C(703)) {
        OresSourceActorIdentity current{};
        if (!oreslang_source_actor_current_identity(&current)
                || current.logical_id == 0
                || current.scheduler_domain == 0
                || current.scheduler_generation == 0) std::abort();
        if (payload == INT64_C(701)) {
            publisher_turn_generation.store(
                    current.scheduler_generation, std::memory_order_relaxed);
            publisher_turn_id.store(
                    current.logical_id, std::memory_order_release);
        } else if (payload == INT64_C(702)) {
            subscriber_turn_generation.store(
                    current.scheduler_generation, std::memory_order_relaxed);
            subscriber_turn_id.store(
                    current.logical_id, std::memory_order_release);
        } else {
            auto* pubsub = live_pubsub.load(std::memory_order_acquire);
            if (!pubsub) std::abort();
            OresNativePubSubTryPublishResultV1 receipt{};
            auto status = ores_native_actor_pubsub_try_publish_i64_result_v1(
                    pubsub, 1, "source-identity", current, INT64_C(73),
                    &receipt);
            turn_publish_tag.store(receipt.tag, std::memory_order_relaxed);
            turn_publish_sequence.store(
                    receipt.receipt.sequence, std::memory_order_relaxed);
            turn_publish_status.store(
                    static_cast<int>(status), std::memory_order_release);
        }
    }
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

    // A host/main thread is NOT an authenticated compiler-generated actor
    // turn even if it holds an otherwise valid opaque ActorRef.
    OresSourceActorIdentity denied_host{};
    denied_host.logical_id = UINT64_C(99999);
    check(!oreslang_source_actor_current_identity(&denied_host),
          "host thread forged a source actor turn");
    check(denied_host.logical_id == 0
            && denied_host.scheduler_generation == 0,
          "failed source turn identity leaked stale caller bytes");
    check(!oreslang_source_actor_current_identity(nullptr),
          "null current-identity output was accepted");

    oreslang_source_actor_send(publisher, INT64_C(701));
    oreslang_source_actor_send(subscriber, INT64_C(702));
    require_probe(publisher_turn_id, "publisher's source turn was not observed");
    require_probe(subscriber_turn_id, "subscriber's source turn was not observed");
    check(publisher_turn_id.load() == publisher_id.logical_id
            && publisher_turn_generation.load() == publisher_id.scheduler_generation,
          "publisher current-turn identity differs from trusted host snapshot");
    check(subscriber_turn_id.load() == subscriber_id.logical_id
            && subscriber_turn_generation.load() == subscriber_id.scheduler_generation,
          "subscriber current-turn identity differs from trusted host snapshot");

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

    // The same source actor callback (not the outer host) obtains its
    // publisher identity from live scheduler TLS and crosses the actual
    // native tagged Result pub/sub admission ABI. No actor pointer or
    // arbitrary OresSourceActorIdentity is handed in by source code.
    live_pubsub.store(pubsub, std::memory_order_release);
    oreslang_source_actor_send(publisher, INT64_C(703));
    const auto typed_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
    while (turn_publish_status.load(std::memory_order_acquire) == -1
            && std::chrono::steady_clock::now() < typed_deadline) {
        sched_yield();
    }
    check(turn_publish_status.load(std::memory_order_acquire)
                    == ORES_NATIVE_PUBSUB_OK
            && turn_publish_tag.load() == ORES_NATIVE_PUBSUB_TRY_OK
            && turn_publish_sequence.load() == 2,
          "native typed Result publication from authenticated actor turn failed");
    value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
              pubsub, 1, "source-identity", subscriber_id, &value)
              == ORES_NATIVE_PUBSUB_OK && value == INT64_C(73),
          "native typed publication did not deliver to the subscribed actor");
    // A valid scheduler-minted turn is necessary but NOT sufficient for a
    // publication. Revoking group membership must deny the same live actor
    // and refuse to create a typed receipt; rejoin restores only a fresh
    // authorized membership, not old unguarded publication privileges.
    check(ores_native_actor_pubsub_leave(
              pubsub, 1, publisher_id) == ORES_NATIVE_PUBSUB_OK,
          "publisher leave admission failed");
    turn_publish_status.store(-1, std::memory_order_release);
    turn_publish_tag.store(-1, std::memory_order_relaxed);
    turn_publish_sequence.store(0, std::memory_order_relaxed);
    oreslang_source_actor_send(publisher, INT64_C(703));
    const auto revoked_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
    while (turn_publish_status.load(std::memory_order_acquire) == -1
            && std::chrono::steady_clock::now() < revoked_deadline) {
        sched_yield();
    }
    check(turn_publish_status.load(std::memory_order_acquire)
                    == ORES_NATIVE_PUBSUB_DENIED_OR_STALE
            && turn_publish_tag.load() == ORES_NATIVE_PUBSUB_TRY_INVALID
            && turn_publish_sequence.load() == 0,
          "revoked source actor turn must not publish or mint a receipt");
    value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
              pubsub, 1, "source-identity", subscriber_id, &value)
              == ORES_NATIVE_PUBSUB_EMPTY,
          "revoked publisher delivered unauthorized DATA");

    check(ores_native_actor_pubsub_join(
              pubsub, 1, publisher_id) == ORES_NATIVE_PUBSUB_OK,
          "publisher rejoin admission failed");
    turn_publish_status.store(-1, std::memory_order_release);
    oreslang_source_actor_send(publisher, INT64_C(703));
    const auto rejoined_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
    while (turn_publish_status.load(std::memory_order_acquire) == -1
            && std::chrono::steady_clock::now() < rejoined_deadline) {
        sched_yield();
    }
    check(turn_publish_status.load(std::memory_order_acquire)
                    == ORES_NATIVE_PUBSUB_OK
            && turn_publish_tag.load() == ORES_NATIVE_PUBSUB_TRY_OK
            && turn_publish_sequence.load() == 3,
          "rejoined source actor failed contiguous sequence publication");
    value = 0;
    check(ores_native_actor_pubsub_try_read_i64(
              pubsub, 1, "source-identity", subscriber_id, &value)
              == ORES_NATIVE_PUBSUB_OK && value == INT64_C(73),
          "rejoined publisher was not delivered exactly once");
    live_pubsub.store(nullptr, std::memory_order_release);

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
