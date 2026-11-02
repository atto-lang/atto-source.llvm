#include "oreslang/runtime/NativeActorPubSubABI.h"

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"
#include "oreslang/runtime/NativeActorPubSub.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
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
    NativeActorPubSubAdmission<std::string> utf8_bus;
    std::mutex groups_mutex;
    std::map<std::uint64_t, NativeActorMembershipRegistry::JoinCapability> groups;
    // One topic name has one fixed payload type for its group lifetime.
    // Protected by groups_mutex alongside group creation/closure.
    std::map<std::pair<std::uint64_t, std::string>, char> topic_types;

    OresNativeActorPubSubRuntime(
            std::uint64_t runtime_domain,
            std::size_t max_groups,
            std::size_t max_groups_per_actor,
            std::size_t max_topics,
            std::size_t max_subscriptions_per_actor)
        : memberships(runtime_domain, max_groups, max_groups_per_actor),
          i64_bus(memberships, max_topics, max_subscriptions_per_actor),
          utf8_bus(memberships, max_topics, max_subscriptions_per_actor) {}
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

// Strict UTF-8: reject overlong code points, surrogates and >U+10FFFF.
// The compiler/runtime owns these byte spans and never passes a guest pointer
// through as a long-lived capability.
bool valid_utf8_bytes(const char* text, std::size_t length) noexcept {
    if (!text && length != 0) return false;
    std::size_t i = 0;
    while (i < length) {
        const unsigned char a = static_cast<unsigned char>(text[i++]);
        if (a < 0x80) continue;
        int tails = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (a >= 0xc2 && a <= 0xdf) {
            tails = 1; value = a & 0x1f; minimum = 0x80;
        } else if (a >= 0xe0 && a <= 0xef) {
            tails = 2; value = a & 0x0f; minimum = 0x800;
        } else if (a >= 0xf0 && a <= 0xf4) {
            tails = 3; value = a & 0x07; minimum = 0x10000;
        } else return false;
        if (static_cast<std::size_t>(tails) > length - i) return false;
        for (int j = 0; j < tails; ++j) {
            const unsigned char b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xc0) != 0x80) return false;
            value = (value << 6) | (b & 0x3f);
        }
        if (value < minimum || value > 0x10ffff
                || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
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
    return valid_utf8_bytes(name, length);
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

void require_topic_type_locked(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        char kind) {
    auto it = runtime->topic_types.find(
        std::make_pair(group_id, std::string(topic_name)));
    if (it != runtime->topic_types.end() && it->second != kind)
        throw std::logic_error(
            "native ActorGroup topic cannot change its payload type");
}

template<class Receipt>
OresNativePubSubPublishReceipt output_receipt(const Receipt& receipt) {
    return OresNativePubSubPublishReceipt{
        static_cast<std::uint64_t>(receipt.subscribers),
        static_cast<std::uint64_t>(receipt.accepted),
        static_cast<std::uint64_t>(receipt.dropped),
        static_cast<std::uint64_t>(receipt.coalesced),
        receipt.backpressured ? 1 : 0
    };
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
        runtime->utf8_bus.remove_actor(
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
        (void)runtime->utf8_bus.close_group(group_id);
        for (auto it = runtime->topic_types.begin();
                it != runtime->topic_types.end();) {
            if (it->first.first == group_id)
                it = runtime->topic_types.erase(it);
            else ++it;
        }
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
        runtime->utf8_bus.remove_actor_from_group(
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
        std::lock_guard<std::mutex> lock(runtime->groups_mutex);
        require_topic_type_locked(runtime, group_id, topic_name, 'i');
        runtime->i64_bus.define_topic(
            group_id,
            std::string(topic_name),
            policy(topic_policy),
            capacity);
        runtime->topic_types.emplace(
            std::make_pair(group_id, std::string(topic_name)), 'i');
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
        *out_receipt = output_receipt(receipt);
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

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_define_utf8_topic(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresNativePubSubPolicy topic_policy,
        std::size_t capacity) {
    if (!runtime || group_id == 0 || !valid_topic_name(topic_name)
            || capacity == 0) return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        std::lock_guard<std::mutex> lock(runtime->groups_mutex);
        require_topic_type_locked(runtime, group_id, topic_name, 's');
        using Policy =
            NativeActorPubSubAdmission<std::string>::DeliveryPolicy;
        Policy converted;
        switch (topic_policy) {
            case ORES_NATIVE_PUBSUB_RELIABLE: converted = Policy::Reliable; break;
            case ORES_NATIVE_PUBSUB_LATEST: converted = Policy::Latest; break;
            case ORES_NATIVE_PUBSUB_LOSSY: converted = Policy::Lossy; break;
            default: throw std::invalid_argument("invalid native pub-sub policy");
        }
        runtime->utf8_bus.define_topic(
            group_id, std::string(topic_name), converted, capacity);
        runtime->topic_types.emplace(
            std::make_pair(group_id, std::string(topic_name)), 's');
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_subscribe_utf8(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity subscriber) {
    if (!valid_runtime_and_actor(runtime, subscriber) || group_id == 0
            || !valid_topic_name(topic_name)) return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        (void)runtime->utf8_bus.subscribe(
            group_id, topic_name,
            subscriber.logical_id, subscriber.scheduler_generation);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_unsubscribe_utf8(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity subscriber) {
    if (!valid_runtime_and_actor(runtime, subscriber) || group_id == 0
            || !valid_topic_name(topic_name)) return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        // An old incarnation is not allowed to unsubscribe a replacement.
        if (!runtime->memberships.contains(
                group_id, subscriber.logical_id, subscriber.scheduler_generation))
            throw std::logic_error("subscription is no longer authorized");
        if (!runtime->utf8_bus.unsubscribe(
                group_id, topic_name,
                subscriber.logical_id, subscriber.scheduler_generation))
            throw std::logic_error("subscription is already closed");
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_publish_utf8(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity publisher,
        const char* payload,
        std::size_t payload_length,
        OresNativePubSubPublishReceipt* out_receipt) {
    if (!valid_runtime_and_actor(runtime, publisher) || group_id == 0
            || !valid_topic_name(topic_name) || !out_receipt
            || payload_length > NativeActorPubSubAdmission<std::string>::DEFAULT_EDGE_BYTES
            || !valid_utf8_bytes(payload, payload_length))
        return ORES_NATIVE_PUBSUB_INVALID;
    return guarded([&] {
        const std::string frozen(
            payload ? payload : "", payload_length);
        const auto receipt = runtime->utf8_bus.publish(
            group_id, topic_name,
            publisher.logical_id, publisher.scheduler_generation, frozen);
        *out_receipt = output_receipt(receipt);
    });
}

extern "C" OresNativePubSubStatus
ores_native_actor_pubsub_try_read_utf8(
        OresNativeActorPubSubRuntime* runtime,
        std::uint64_t group_id,
        const char* topic_name,
        OresSourceActorIdentity subscriber,
        char* out_buffer,
        std::size_t out_capacity,
        std::size_t* out_length) {
    if (!valid_runtime_and_actor(runtime, subscriber) || group_id == 0
            || !valid_topic_name(topic_name) || !out_length
            || (out_capacity != 0 && !out_buffer))
        return ORES_NATIVE_PUBSUB_INVALID;
    *out_length = 0;
    using Admission = NativeActorPubSubAdmission<std::string>;
    Admission::ReadIntoStatus read = Admission::ReadIntoStatus::Empty;
    const OresNativePubSubStatus status = guarded([&] {
        read = runtime->utf8_bus.try_read_into(
            group_id, topic_name,
            subscriber.logical_id, subscriber.scheduler_generation,
            [&](const std::string& value) {
                *out_length = value.size();
                if (out_capacity < value.size()) return false;
                if (!value.empty())
                    std::memcpy(out_buffer, value.data(), value.size());
                return true;
            });
    });
    if (status != ORES_NATIVE_PUBSUB_OK) return status;
    switch (read) {
        case Admission::ReadIntoStatus::Empty:
            return ORES_NATIVE_PUBSUB_EMPTY;
        case Admission::ReadIntoStatus::InsufficientCapacity:
            return ORES_NATIVE_PUBSUB_RESOURCE_EXHAUSTED;
        case Admission::ReadIntoStatus::Consumed:
            return ORES_NATIVE_PUBSUB_OK;
    }
    return ORES_NATIVE_PUBSUB_INTERNAL_ERROR;
}
