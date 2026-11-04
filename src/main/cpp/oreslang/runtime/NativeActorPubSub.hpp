#pragma once

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Shared C++/C-ABI topic identity gate for portable ActorGroup PubSub v1.
// No locale, Unicode normalization, allocation or implicit case folding.
// Exact grammar: ^[A-Za-z][A-Za-z0-9._/-]{0,127}$ (1..128 bytes).
inline bool valid_portable_topic_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        const bool letter = (ch >= 'A' && ch <= 'Z')
            || (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (i == 0 ? !letter
                : !(letter || digit || ch == '.' || ch == '_'
                        || ch == '/' || ch == '-'))
            return false;
    }
    return true;
}

template<class T>
struct NativeActorPubSubPayloadBytes final {
    static std::size_t size(const T&) noexcept { return sizeof(T); }
};

template<>
struct NativeActorPubSubPayloadBytes<std::string> final {
    static std::size_t size(const std::string& value) noexcept {
        return value.size();
    }
};

/*
 * Runtime-wide resource ledger shared by every typed pub/sub lane. Without a
 * shared ledger, mixing Topic<int> and Topic<String> could multiply the
 * advertised graph byte, preallocated-slot, and per-actor subscription
 * limits by the number of payload types.
 *
 * Lock order is always membership -> typed bus -> this ledger. The ledger
 * never calls back into a bus, so typed lanes may safely share it.
 */
class NativeActorPubSubResourceLedger final {
public:
    NativeActorPubSubResourceLedger(
            std::size_t max_subscriptions_per_actor,
            std::size_t max_total_bytes,
            std::size_t max_total_queue_slots)
        : max_subscriptions_per_actor_(max_subscriptions_per_actor),
          max_total_bytes_(max_total_bytes),
          max_total_queue_slots_(max_total_queue_slots) {
        if (max_subscriptions_per_actor_ == 0
                || max_total_bytes_ == 0
                || max_total_queue_slots_ == 0)
            throw std::invalid_argument("invalid native pub-sub shared limits");
    }

    void reserve_subscription(
            std::uint64_t actor_id,
            std::uint64_t generation,
            std::size_t queue_slots) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (queue_slots == 0
                || allocated_queue_slots_ > max_total_queue_slots_
                || queue_slots > max_total_queue_slots_ - allocated_queue_slots_)
            throw std::logic_error(
                "native pub-sub shared queue-slot budget exceeded");

        const ActorKey actor{actor_id, generation};
        auto it = subscriptions_.find(actor);
        const std::size_t current =
            it == subscriptions_.end() ? 0 : it->second;
        if (current >= max_subscriptions_per_actor_)
            throw std::logic_error(
                "native pub-sub shared actor subscription quota exceeded");

        if (it == subscriptions_.end())
            it = subscriptions_.emplace(actor, 0).first;
        ++it->second;
        allocated_queue_slots_ += queue_slots;
    }

    void release_subscription(
            std::uint64_t actor_id,
            std::uint64_t generation,
            std::size_t queue_slots) {
        std::lock_guard<std::mutex> guard(mutex_);
        const ActorKey actor{actor_id, generation};
        auto it = subscriptions_.find(actor);
        if (it == subscriptions_.end() || it->second == 0
                || queue_slots > allocated_queue_slots_)
            throw std::logic_error("native pub-sub shared ledger underflow");
        if (--it->second == 0) subscriptions_.erase(it);
        allocated_queue_slots_ -= queue_slots;
    }

    bool try_reserve_bytes(std::size_t bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (queued_payload_bytes_ > max_total_bytes_
                || bytes > max_total_bytes_ - queued_payload_bytes_)
            return false;
        queued_payload_bytes_ += bytes;
        return true;
    }

    bool try_replace_bytes(std::size_t remove_bytes, std::size_t add_bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (remove_bytes > queued_payload_bytes_)
            throw std::logic_error("native pub-sub shared byte ledger underflow");
        const std::size_t base = queued_payload_bytes_ - remove_bytes;
        if (base > max_total_bytes_ || add_bytes > max_total_bytes_ - base)
            return false;
        queued_payload_bytes_ = base + add_bytes;
        return true;
    }

    void release_bytes(std::size_t bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (bytes > queued_payload_bytes_)
            throw std::logic_error("native pub-sub shared byte ledger underflow");
        queued_payload_bytes_ -= bytes;
    }

    std::size_t queued_payload_bytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return queued_payload_bytes_;
    }

    std::size_t allocated_queue_slots() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return allocated_queue_slots_;
    }

private:
    using ActorKey = std::pair<std::uint64_t, std::uint64_t>;

    const std::size_t max_subscriptions_per_actor_;
    const std::size_t max_total_bytes_;
    const std::size_t max_total_queue_slots_;
    mutable std::mutex mutex_;
    std::map<ActorKey, std::size_t> subscriptions_;
    std::size_t queued_payload_bytes_ = 0;
    std::size_t allocated_queue_slots_ = 0;
};

/**
 * Host-side bounded admission substrate for ActorGroup pub/sub.
 *
 * This is deliberately not the source-language lowering. Source Topic<T> /
 * Subscription<T> stays fail-closed until the compiler can bind actor
 * continuations and cancellation to this admission layer.
 *
 * RELIABLE publication is atomic at the queue-admission boundary: payload
 * allocation/freeze happens once before commit, and subscriber queues are
 * preallocated bounded rings. After admission checks pass, fanout therefore
 * performs no per-subscriber allocation that could leave a partial publish.
 */
template<class T>
class NativeActorPubSubAdmission final {
    static_assert(
        std::is_trivially_copyable_v<T> || std::is_same_v<T, std::string>,
        "native pub-sub payload must be fixed-size trivially copyable data or std::string");

public:
    enum class DeliveryPolicy { Reliable, Latest, Lossy };

    static constexpr std::size_t DEFAULT_EDGE_BYTES =
        std::size_t{16} * 1024 * 1024;
    static constexpr std::size_t DEFAULT_GRAPH_BYTES =
        std::size_t{64} * 1024 * 1024;
    static constexpr std::size_t DEFAULT_MAX_QUEUE_CAPACITY = 65536;
    static constexpr std::size_t DEFAULT_TOTAL_QUEUE_SLOTS = 1u << 20;

    struct PublishReceipt {
        std::size_t subscribers = 0;
        std::size_t accepted = 0;
        std::size_t dropped = 0;
        std::size_t coalesced = 0;
        bool backpressured = false;
        // Host C++ result only; the C ABI remains unchanged until versioned.
        std::uint64_t sequence = 0;
    };

    NativeActorPubSubAdmission(
            const NativeActorMembershipRegistry& memberships,
            std::size_t max_topics,
            std::size_t max_subscriptions_per_actor,
            std::size_t max_bytes_per_subscription = DEFAULT_EDGE_BYTES,
            std::size_t max_total_bytes = DEFAULT_GRAPH_BYTES,
            std::size_t max_queue_capacity = DEFAULT_MAX_QUEUE_CAPACITY,
            std::size_t max_total_queue_slots = DEFAULT_TOTAL_QUEUE_SLOTS,
            std::shared_ptr<NativeActorPubSubResourceLedger> shared_resources = {})
        : memberships_(memberships),
          max_topics_(max_topics),
          max_bytes_per_subscription_(max_bytes_per_subscription),
          max_queue_capacity_(max_queue_capacity),
          resources_(shared_resources ? std::move(shared_resources)
              : std::make_shared<NativeActorPubSubResourceLedger>(
                    max_subscriptions_per_actor,
                    max_total_bytes,
                    max_total_queue_slots)) {
        if (max_topics_ == 0 || max_subscriptions_per_actor == 0
                || max_bytes_per_subscription_ == 0
                || max_total_bytes == 0
                || max_queue_capacity_ == 0
                || max_total_queue_slots == 0)
            throw std::invalid_argument("invalid native pub-sub limits");
    }

    void define_topic(
            std::uint64_t group_id,
            std::string name,
            DeliveryPolicy policy,
            std::size_t capacity) {
        validate_topic(group_id, name, policy, capacity);
        memberships_.with_locked_view([&](const auto& memberships) {
            if (!memberships.group_exists(group_id))
                throw std::logic_error("unknown or closed ActorGroup");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicKey key{group_id, std::move(name)};
            auto existing = topics_.find(key);
            if (existing != topics_.end()) {
                if (existing->second.policy != policy
                        || existing->second.capacity != capacity)
                    throw std::logic_error(
                        "topic already defined with different policy/capacity");
                return;
            }
            if (topics_.size() >= max_topics_)
                throw std::logic_error("native pub-sub topic capacity exceeded");
            topics_.emplace(
                std::move(key),
                TopicRecord{policy, capacity, {}});
        });
    }

    bool subscribe(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t actor_id,
            std::uint64_t generation) {
        return memberships_.with_locked_view([&](const auto& memberships) {
            if (!memberships.contains(group_id, actor_id, generation))
                throw std::logic_error(
                    "subscription requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            ActorKey actor{actor_id, generation};
            if (topic.subscribers.find(actor) != topic.subscribers.end())
                return false;
            /*
             * Allocate the bounded ring before reserving shared runtime
             * accounting. A failed allocation changes no quota state.
             */
            Queue queue(topic.capacity);
            resources_->reserve_subscription(
                actor_id, generation, topic.capacity);
            try {
                auto inserted =
                    topic.subscribers.emplace(actor, std::move(queue));
                if (!inserted.second)
                    throw std::logic_error(
                        "duplicate native pub-sub subscription");
            } catch (...) {
                resources_->release_subscription(
                    actor_id, generation, topic.capacity);
                throw;
            }
            return true;
        });
    }

    bool unsubscribe(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t actor_id,
            std::uint64_t generation) {
        // The membership check and queue-edge removal must be atomic with
        // leave/retire/rejoin admission. Checking once in the C wrapper
        // before acquiring the bus lock is an authorization TOCTOU.
        return memberships_.with_locked_view([&](const auto& view) {
            if (!view.contains(group_id, actor_id, generation))
                throw std::logic_error(
                    "unsubscribe requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            auto topic = topics_.find(TopicKey{group_id, name});
            if (topic == topics_.end()) return false;
            return erase_subscription_locked(
                topic->second, ActorKey{actor_id, generation});
        });
    }

    PublishReceipt publish(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t publisher_id,
            std::uint64_t publisher_generation,
            const T& value) {
        return publish_internal(
            group_id, name, publisher_id, publisher_generation, value, false);
    }

    // Host-only nonblocking RELIABLE admission. Empty means Full; rejected
    // identity, invalid payload, unsupported delivery policy and closed
    // group/topic remain explicit exceptions, not silent empty results.
    // This is NOT source-level Topic<T>.try_publish guest lowering.
    std::optional<PublishReceipt> try_publish_reliable(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t publisher_id,
            std::uint64_t publisher_generation,
            const T& value) {
        auto receipt = publish_internal(
            group_id, name, publisher_id, publisher_generation, value, true);
        if (receipt.backpressured) return std::nullopt;
        return receipt;
    }

private:
    PublishReceipt publish_internal(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t publisher_id,
            std::uint64_t publisher_generation,
            const T& value,
            bool require_reliable) {
        /*
         * Reject unauthorized publishers before allocating a frozen payload.
         * The locked view below revalidates membership at the actual commit
         * boundary, so leave/retire can still win between these two checks.
         */
        if (!memberships_.contains(
                group_id, publisher_id, publisher_generation))
            throw std::logic_error(
                "publication requires live ActorGroup membership");

        const std::size_t payload_bytes =
            NativeActorPubSubPayloadBytes<T>::size(value);
        if (payload_bytes > max_bytes_per_subscription_) {
            throw std::logic_error(
                "native pub-sub payload exceeds per-subscription byte quota");
        }

        /*
         * Freeze/copy once before taking the membership/topic commit locks.
         * Allocation failure therefore changes no subscriber queue.
         */
        std::shared_ptr<const T> frozen =
            std::make_shared<const T>(value);

        return memberships_.with_locked_view([&](const auto& memberships) {
            if (!memberships.contains(
                    group_id, publisher_id, publisher_generation))
                throw std::logic_error(
                    "publication requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            if (require_reliable && topic.policy != DeliveryPolicy::Reliable)
                throw std::invalid_argument(
                    "try_publish_reliable requires a RELIABLE topic");
            prune_stale_locked(group_id, topic, memberships);
            return publish_locked(
                topic, std::move(frozen), payload_bytes);
        });
    }

public:
    // No publish_system bypass on the general admission surface: the host
    // must supply authenticated supervisor/CONTROL authority through a
    // separately reviewed, non-guest API before system publications exist.

    std::optional<T> try_read(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t actor_id,
            std::uint64_t generation) {
        return memberships_.with_locked_view([&](const auto& memberships)
                -> std::optional<T> {
            if (!memberships.contains(group_id, actor_id, generation))
                throw std::logic_error(
                    "subscription read requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            auto subscriber =
                topic.subscribers.find(ActorKey{actor_id, generation});
            if (subscriber == topic.subscribers.end())
                throw std::logic_error("actor is not subscribed to topic");
            Queue& queue = subscriber->second;
            if (queue.empty()) return std::nullopt;

            /*
             * Copy before mutating queue/accounting. A future std::string
             * allocation failure leaves the event fully queued.
             */
            const Slot& slot = queue.front();
            T value = *slot.value;
            const std::size_t bytes = slot.bytes;
            queue.pop();
            resources_->release_bytes(bytes);
            return value;
        });
    }

    // Runtime-only bounded UTF-8/other typed delivery sink. The caller sees
    // a value under the same membership + queue lock that decides whether
    // to consume it. An undersized output buffer MUST NOT dequeue the value.
    // Guest code never receives this sink or the internal queue reference.
    enum class ReadIntoStatus { Empty, Consumed, InsufficientCapacity };

    template<class Sink>
    ReadIntoStatus try_read_into(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t actor_id,
            std::uint64_t generation,
            Sink&& sink) {
        return memberships_.with_locked_view([&](const auto& membership_view) {
            if (!membership_view.contains(group_id, actor_id, generation))
                throw std::logic_error(
                    "subscription read requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            auto subscriber =
                topic.subscribers.find(ActorKey{actor_id, generation});
            if (subscriber == topic.subscribers.end())
                throw std::logic_error("actor is not subscribed to topic");
            Queue& queue = subscriber->second;
            if (queue.empty()) return ReadIntoStatus::Empty;
            const Slot& slot = queue.front();
            if (!std::forward<Sink>(sink)(*slot.value))
                return ReadIntoStatus::InsufficientCapacity;
            const std::size_t bytes = slot.bytes;
            queue.pop();
            resources_->release_bytes(bytes);
            return ReadIntoStatus::Consumed;
        });
    }

    void remove_actor_from_group(
            std::uint64_t group_id,
            std::uint64_t actor_id,
            std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        ActorKey actor{actor_id, generation};
        for (auto& entry : topics_) {
            if (entry.first.group_id != group_id) continue;
            (void)erase_subscription_locked(entry.second, actor);
        }
    }

    void remove_actor(std::uint64_t actor_id, std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        ActorKey actor{actor_id, generation};
        for (auto& entry : topics_)
            (void)erase_subscription_locked(entry.second, actor);
    }

    std::size_t close_group(std::uint64_t group_id) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::size_t removed = 0;
        for (auto it = topics_.begin(); it != topics_.end();) {
            if (it->first.group_id != group_id) {
                ++it;
                continue;
            }
            for (const auto& subscriber : it->second.subscribers) {
                resources_->release_bytes(subscriber.second.payload_bytes);
                resources_->release_subscription(
                    subscriber.first.actor_id,
                    subscriber.first.generation,
                    subscriber.second.slots.size());
            }
            it = topics_.erase(it);
            ++removed;
        }
        return removed;
    }

    std::size_t subscriber_count(
            std::uint64_t group_id,
            const std::string& name) {
        return memberships_.with_locked_view([&](const auto& memberships) {
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            prune_stale_locked(group_id, topic, memberships);
            return topic.subscribers.size();
        });
    }

    std::size_t queued_payload_bytes() const {
        return resources_->queued_payload_bytes();
    }

    std::size_t allocated_queue_slots() const {
        return resources_->allocated_queue_slots();
    }

private:
    struct ActorKey {
        std::uint64_t actor_id;
        std::uint64_t generation;

        friend bool operator<(const ActorKey& left, const ActorKey& right) {
            return std::tie(left.actor_id, left.generation)
                < std::tie(right.actor_id, right.generation);
        }
    };

    struct TopicKey {
        std::uint64_t group_id;
        std::string name;

        friend bool operator<(const TopicKey& left, const TopicKey& right) {
            return std::tie(left.group_id, left.name)
                < std::tie(right.group_id, right.name);
        }
    };

    struct Slot {
        std::shared_ptr<const T> value;
        std::size_t bytes = 0;
    };

    struct Queue {
        explicit Queue(std::size_t capacity)
            : slots(capacity) {}

        Queue(Queue&&) noexcept = default;
        Queue& operator=(Queue&&) noexcept = default;
        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        bool empty() const noexcept { return size == 0; }
        bool full() const noexcept { return size == slots.size(); }

        const Slot& front() const {
            if (empty()) throw std::logic_error("native pub-sub queue empty");
            return slots[head];
        }

        void push(
                const std::shared_ptr<const T>& value,
                std::size_t bytes) {
            if (full()) throw std::logic_error("native pub-sub queue full");
            const std::size_t index = (head + size) % slots.size();
            slots[index].value = value;
            slots[index].bytes = bytes;
            ++size;
            payload_bytes += bytes;
        }

        std::size_t replace_latest(
                const std::shared_ptr<const T>& value,
                std::size_t bytes) {
            if (empty()) {
                push(value, bytes);
                return 0;
            }
            if (size != 1)
                throw std::logic_error("LATEST queue contains multiple values");
            const std::size_t old = slots[head].bytes;
            slots[head].value = value;
            slots[head].bytes = bytes;
            payload_bytes = bytes;
            return old;
        }

        void pop() {
            if (empty()) throw std::logic_error("native pub-sub queue empty");
            payload_bytes -= slots[head].bytes;
            slots[head].value.reset();
            slots[head].bytes = 0;
            head = (head + 1) % slots.size();
            --size;
        }

        std::vector<Slot> slots;
        std::size_t head = 0;
        std::size_t size = 0;
        std::size_t payload_bytes = 0;
    };

    struct TopicRecord {
        DeliveryPolicy policy;
        std::size_t capacity;
        std::map<ActorKey, Queue> subscribers;
        std::uint64_t last_sequence = 0;
    };

    void validate_topic(
            std::uint64_t group_id,
            const std::string& name,
            DeliveryPolicy policy,
            std::size_t capacity) const {
        if (group_id == 0 || !valid_portable_topic_name(name)
                || capacity == 0 || capacity > max_queue_capacity_)
            throw std::invalid_argument("invalid native pub-sub topic");
        if (policy == DeliveryPolicy::Latest && capacity != 1)
            throw std::invalid_argument(
                "LATEST native pub-sub topic capacity must be one");
    }

    static bool would_exceed(
            std::size_t current,
            std::size_t add,
            std::size_t limit) noexcept {
        return current > limit || add > limit - current;
    }

    TopicRecord& require_topic_locked(
            std::uint64_t group_id,
            const std::string& name) {
        auto it = topics_.find(TopicKey{group_id, name});
        if (it == topics_.end())
            throw std::logic_error("unknown native pub-sub topic");
        return it->second;
    }

    bool erase_subscription_locked(
            TopicRecord& topic,
            const ActorKey& actor) {
        auto subscriber = topic.subscribers.find(actor);
        if (subscriber == topic.subscribers.end()) return false;
        resources_->release_bytes(subscriber->second.payload_bytes);
        resources_->release_subscription(
            actor.actor_id, actor.generation, subscriber->second.slots.size());
        topic.subscribers.erase(subscriber);
        return true;
    }

    template<class MembershipView>
    void prune_stale_locked(
            std::uint64_t group_id,
            TopicRecord& topic,
            const MembershipView& memberships) {
        for (auto it = topic.subscribers.begin();
             it != topic.subscribers.end();) {
            if (memberships.contains(
                    group_id,
                    it->first.actor_id,
                    it->first.generation)) {
                ++it;
                continue;
            }
            const ActorKey stale = it->first;
            resources_->release_bytes(it->second.payload_bytes);
            resources_->release_subscription(
                stale.actor_id, stale.generation, it->second.slots.size());
            it = topic.subscribers.erase(it);
        }
    }

    PublishReceipt publish_locked(
            TopicRecord& topic,
            std::shared_ptr<const T> frozen,
            std::size_t payload_bytes) {
        PublishReceipt receipt;
        receipt.subscribers = topic.subscribers.size();
        // Unsigned u64 topic-local sequence, never recycled or wrapped.
        // Rejected admission leaves receipt.sequence=0 and state unchanged.
        if (topic.last_sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native pub-sub topic sequence exhausted");
        const std::uint64_t next_sequence = topic.last_sequence + 1;

        if (topic.policy == DeliveryPolicy::Reliable) {
            for (const auto& subscriber : topic.subscribers) {
                const Queue& queue = subscriber.second;
                if (queue.full()
                        || would_exceed(
                            queue.payload_bytes,
                            payload_bytes,
                            max_bytes_per_subscription_)) {
                    receipt.backpressured = true;
                    return receipt;
                }
            }

            if (payload_bytes != 0
                    && receipt.subscribers
                        > std::numeric_limits<std::size_t>::max() / payload_bytes) {
                receipt.backpressured = true;
                return receipt;
            }
            const std::size_t graph_bytes =
                payload_bytes * receipt.subscribers;
            if (!resources_->try_reserve_bytes(graph_bytes)) {
                receipt.backpressured = true;
                return receipt;
            }

            for (auto& subscriber : topic.subscribers) {
                subscriber.second.push(frozen, payload_bytes);
                ++receipt.accepted;
            }
            topic.last_sequence = next_sequence;
            receipt.sequence = next_sequence;
            return receipt;
        }

        if (topic.policy == DeliveryPolicy::Latest) {
            /*
             * LATEST promises the newest value, so byte-budget failure is an
             * explicit resource rejection, never a silent drop of the new
             * publication. Compute the complete post-replacement graph budget
             * before modifying any queue.
             */
            std::size_t remove_bytes = 0;
            std::size_t add_bytes = 0;
            for (const auto& subscriber : topic.subscribers) {
                const Queue& queue = subscriber.second;
                const std::size_t old =
                    queue.empty() ? 0 : queue.front().bytes;
                if (remove_bytes
                        > std::numeric_limits<std::size_t>::max() - old
                        || add_bytes
                        > std::numeric_limits<std::size_t>::max() - payload_bytes)
                    throw std::overflow_error(
                        "LATEST publication byte accounting overflow");
                remove_bytes += old;
                add_bytes += payload_bytes;
            }
            if (!resources_->try_replace_bytes(remove_bytes, add_bytes))
                throw std::logic_error(
                    "LATEST publication exceeds shared graph byte quota");

            for (auto& subscriber : topic.subscribers) {
                Queue& queue = subscriber.second;
                const bool had_value = !queue.empty();
                (void)queue.replace_latest(frozen, payload_bytes);
                if (had_value) ++receipt.coalesced;
                ++receipt.accepted;
            }
            topic.last_sequence = next_sequence;
            receipt.sequence = next_sequence;
            return receipt;
        }

        for (auto& subscriber : topic.subscribers) {
            Queue& queue = subscriber.second;
            if (queue.full()
                    || would_exceed(
                        queue.payload_bytes,
                        payload_bytes,
                        max_bytes_per_subscription_)
                    || !resources_->try_reserve_bytes(payload_bytes)) {
                ++receipt.dropped;
                continue;
            }
            queue.push(frozen, payload_bytes);
            ++receipt.accepted;
        }
        // LOSSY counts a successfully processed publication attempt even
        // when all eligible recipient queues drop the delivery.
        topic.last_sequence = next_sequence;
        receipt.sequence = next_sequence;
        return receipt;
    }

    const NativeActorMembershipRegistry& memberships_;
    const std::size_t max_topics_;
    const std::size_t max_bytes_per_subscription_;
    const std::size_t max_queue_capacity_;
    std::shared_ptr<NativeActorPubSubResourceLedger> resources_;
    mutable std::mutex mutex_;
    std::map<TopicKey, TopicRecord> topics_;
};

} // namespace oreslang::runtime
