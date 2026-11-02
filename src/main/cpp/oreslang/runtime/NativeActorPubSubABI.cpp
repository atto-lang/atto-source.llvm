#include "oreslang/runtime/NativeActorPubSubABI.h"

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include "oreslang/runtime/NativeActorPubSub.hpp"

#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::NativeActorPubSubAdmission;

struct OresNativeActorPubSubRuntime {
    NativeActorMembershipRegistry memberships;
    NativeActorPubSubAdmission<std::int64_t> i64_bus;
    std::mutex groups_mutex;
    std::map<std::uint64_t, NativeActorMembershipRegistry::JoinCapability> groups;

    OresNativeActorPubSubRuntime(
            std::uint64_t runtime_domain,
            std::size_t max_groups,
            std::size_t max_groups_per_actor,
            std::size_t max_topics,
            std::size_t max_subscriptions_per_actor)
        : memberships(runtime_domain, max_groups, max_groups_per_actor),
          i64_bus(memberships, max_topics, max_subscriptions_per_actor) {}
};

namespace {

bool valid_runtime_and_actor(
        OresNativeActorPubSubRuntime* runtime,
        const OresSourceActorIdentity& actor) {
    return runtime
        && actor.logical_id != 0
        && actor.scheduler_domain == runtime->memberships.runtime_id()
        && actor.scheduler_generation != 0;
}

bool valid_topic_name(const char* name) {
    if (!name || !*name) return false;
    std::size_t length = 0;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(name);
         *p != 0; ++p) {
        if (++length > 256) return false;
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

NativeActorPubSubAdmission<std::int64_t>::DeliveryPolicy policy(
        OresNativePubSubPolicy value) {
    using Policy = NativeActorPubSubAdmission<std::int64_t>::DeliveryPolicy;
    switch (value) {
        case ORES_NATIVE_PUBSUB_RELIABLE: return Policy::Reliable;
        case ORES_NATIVE_PUBSUB_LATEST: return Policy::Latest;
        case ORES_NATIVE_PUBSUB_LOSSY: return Policy::Lossy;
        default: throw std::invalid_argument("invalid native pub-sub policy");
    }
}

template<class Fn>
OresNativePubSubStatus guarded(Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
        return ORES_NATIVE_PUBSUB_OK;
    } catch (const std::invalid_argument&) {
        return ORES_NATIVE_PUBSUB_INVALID;
    } catch (const std::bad_alloc&) {
        return ORES_NATIVE_PUBSUB_RESOURCE_EXHAUSTED;
    } catch (const std::overflow_error&) {
        return ORES_NATIVE_PUBSUB_RESOURCE_EXHAUSTED;
    } catch (const std::logic_error&) {
        return ORES_NATIVE_PUBSUB_DENIED_OR_STALE;
    } catch (...) {
        return ORES_NATIVE_PUBSUB_INTERNAL_ERROR;
    }
}

} // namespace

extern "C" OresNativeActorPubSubRuntime* ores_native_actor_pubsub_open(
        std::uint64_t runtime_domain,
        std::size_t max_groups,
        std::size_t max_groups_per_actor,
        std::size_t max_topics,
        std::size_t max_subscriptions_per_actor) {
    try {
        return new OresNativeActorPubSubRuntime(
            runtime_domain,
            max_groups,
            max_groups_per_actor,
            max_topics,
            max_subscriptions_per_actor);
    } catch (...) {
        return nullptr;
    }
}

extern "C" void ores_native_actor_pubsub_close(
        OresNativeActorPubSubRuntime* runtime) {
    delete runtime;
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_register_source_actor(
        OresNativeActorPubSubRuntime* runtime,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor))
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        (void)runtime->memberships.register_actor(
            actor.logical_id, actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_retire_source_actor(
        OresNativeActorPubSubRuntime* runtime,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor))
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        /*
         * Membership retirement is the admission barrier. It waits for any
         * publication holding the membership view, then prevents new ones.
         * Queue cleanup occurs only after that barrier.
         */
        if (!runtime->memberships.retire_actor(
                actor.logical_id, actor.scheduler_generation)) {
            throw std::logic_error("unknown or stale actor incarnation");
        }
        runtime->i64_bus.remove_actor(
            actor.logical_id, actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_create_group(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        std::size_t max_members) {
    if (!runtime || group_id == 0 || max_members == 0)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        std::lock_guard<std::mutex> lock(runtime->groups_mutex);
        if (runtime->groups.find(group_id) != runtime->groups.end())
            throw std::logic_error("duplicate ActorGroup");
        auto capability =
            runtime->memberships.create_group(group_id, max_members);
        try {
            runtime->groups.emplace(group_id, std::move(capability));
        } catch (...) {
            (void)runtime->memberships.close_group(group_id);
            throw;
        }
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_close_group(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id) {
    if (!runtime || group_id == 0)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        std::lock_guard<std::mutex> lock(runtime->groups_mutex);
        auto group = runtime->groups.find(group_id);
        if (group == runtime->groups.end())
            throw std::logic_error("unknown ActorGroup");

        /*
         * Close membership first. Any in-flight publish holding the locked
         * membership view finishes before this returns; no new admission can
         * pass afterwards. Topic cleanup is therefore a post-barrier reclaim.
         */
        if (!runtime->memberships.close_group(group_id))
            throw std::logic_error("closed ActorGroup");
        (void)runtime->i64_bus.close_group(group_id);
        runtime->groups.erase(group);
    });
}

extern "C" OresNativePubSubStatus ores_native_actor_pubsub_join(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor) || group_id == 0)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        std::lock_guard<std::mutex> lock(runtime->groups_mutex);
        auto group = runtime->groups.find(group_id);
        if (group == runtime->groups.end())
            throw std::logic_error("unknown ActorGroup");
        (void)runtime->memberships.join(
            group->second, actor.logical_id, actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus ores_native_actor_pubsub_leave(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor) || group_id == 0)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        if (!runtime->memberships.leave(
                group_id, actor.logical_id, actor.scheduler_generation)) {
            throw std::logic_error("membership already absent or stale");
        }
        /*
         * The membership registry is already the barrier. Remove queued topic
         * state only after no publication can authorize this actor again.
         */
        runtime->i64_bus.remove_actor_from_group(
            group_id, actor.logical_id, actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_define_i64_topic(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresNativePubSubPolicy topic_policy,
        std::size_t capacity) {
    if (!runtime || group_id == 0 || !valid_topic_name(topic_name)
            || capacity == 0)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        runtime->i64_bus.define_topic(
            group_id,
            std::string(topic_name),
            policy(topic_policy),
            capacity);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_subscribe_i64(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor) || group_id == 0
            || !valid_topic_name(topic_name))
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        (void)runtime->i64_bus.subscribe(
            group_id,
            topic_name,
            actor.logical_id,
            actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_unsubscribe_i64(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity actor) {
    if (!valid_runtime_and_actor(runtime, actor) || group_id == 0
            || !valid_topic_name(topic_name))
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        (void)runtime->i64_bus.unsubscribe(
            group_id,
            topic_name,
            actor.logical_id,
            actor.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus ores_native_actor_pubsub_publish_i64(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity publisher,
        std::int64_t value,
        OresNativePubSubPublishReceipt* out_receipt) {
    if (!valid_runtime_and_actor(runtime, publisher) || group_id == 0
            || !valid_topic_name(topic_name) || !out_receipt)
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        const auto receipt = runtime->i64_bus.publish(
            group_id,
            topic_name,
            publisher.logical_id,
            publisher.scheduler_generation,
            value);
        *out_receipt = OresNativePubSubPublishReceipt{
            static_cast<std::uint64_t>(receipt.subscribers),
            static_cast<std::uint64_t>(receipt.accepted),
            static_cast<std::uint64_t>(receipt.dropped),
            static_cast<std::uint64_t>(receipt.coalesced),
            receipt.backpressured ? 1 : 0
        };
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_try_read_i64(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity subscriber,
        std::int64_t* out_value) {
    if (!valid_runtime_and_actor(runtime, subscriber) || group_id == 0
            || !valid_topic_name(topic_name) || !out_value)
        return ORES_NATIVE_PUBSUB_INVALID;

    bool empty = false;
    const OresNativePubSubStatus status = guarded([&] {
        const auto value = runtime->i64_bus.try_read(
            group_id,
            topic_name,
            subscriber.logical_id,
            subscriber.scheduler_generation);
        if (!value.has_value()) {
            empty = true;
            return;
        }
        *out_value = value.value();
    });
    if (status != ORES_NATIVE_PUBSUB_OK) return status;
    return empty ? ORES_NATIVE_PUBSUB_EMPTY : ORES_NATIVE_PUBSUB_OK;
}
