#pragma once

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::runtime {

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
    };

    NativeActorPubSubAdmission(
            const NativeActorMembershipRegistry& memberships,
            std::size_t max_topics,
            std::size_t max_subscriptions_per_actor,
            std::size_t max_bytes_per_subscription = DEFAULT_EDGE_BYTES,
            std::size_t max_total_bytes = DEFAULT_GRAPH_BYTES,
            std::size_t max_queue_capacity = DEFAULT_MAX_QUEUE_CAPACITY,
            std::size_t max_total_queue_slots = DEFAULT_TOTAL_QUEUE_SLOTS)
        : memberships_(memberships),
          max_topics_(max_topics),
          max_subscriptions_per_actor_(max_subscriptions_per_actor),
          max_bytes_per_subscription_(max_bytes_per_subscription),
          max_total_bytes_(max_total_bytes),
          max_queue_capacity_(max_queue_capacity),
          max_total_queue_slots_(max_total_queue_slots) {
        if (max_topics_ == 0 || max_subscriptions_per_actor_ == 0
                || max_bytes_per_subscription_ == 0
                || max_total_bytes_ == 0
                || max_queue_capacity_ == 0
                || max_total_queue_slots_ == 0)
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
            std::size_t& count = actor_subscription_counts_[actor];
            if (count >= max_subscriptions_per_actor_)
                throw std::logic_error(
                    "native pub-sub actor subscription quota exceeded");

            /*
             * Allocate the whole bounded ring before publishing this
             * subscription into either index. A failed allocation cannot
             * leave a half-created subscriber or consume quota.
             */
            if (topic.capacity > max_total_queue_slots_ - allocated_queue_slots_)
                throw std::logic_error(
                    "native pub-sub queue-slot budget exceeded");
            Queue queue(topic.capacity);
            topic.subscribers.emplace(actor, std::move(queue));
            allocated_queue_slots_ += topic.capacity;
            ++count;
            return true;
        });
    }

    bool unsubscribe(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t actor_id,
            std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto topic = topics_.find(TopicKey{group_id, name});
        if (topic == topics_.end()) return false;
        return erase_subscription_locked(
            topic->second, ActorKey{actor_id, generation});
    }

    PublishReceipt publish(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t publisher_id,
            std::uint64_t publisher_generation,
            const T& value) {
        const std::size_t payload_bytes =
            NativeActorPubSubPayloadBytes<T>::size(value);
        if (payload_bytes > max_bytes_per_subscription_
                || payload_bytes > max_total_bytes_) {
            throw std::logic_error(
                "native pub-sub payload exceeds configured byte quota");
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
            prune_stale_locked(group_id, topic, memberships);
            return publish_locked(
                topic, std::move(frozen), payload_bytes);
        });
    }

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
            queued_payload_bytes_ -= bytes;
            return value;
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
        actor_subscription_counts_.erase(actor);
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
                queued_payload_bytes_ -= subscriber.second.payload_bytes;
                allocated_queue_slots_ -= subscriber.second.slots.size();
                decrement_actor_subscription_locked(subscriber.first);
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
        std::lock_guard<std::mutex> guard(mutex_);
        return queued_payload_bytes_;
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
    };

    void validate_topic(
            std::uint64_t group_id,
            const std::string& name,
            DeliveryPolicy policy,
            std::size_t capacity) const {
        if (group_id == 0 || name.empty() || name.size() > 256
                || capacity == 0 || capacity > max_queue_capacity_)
            throw std::invalid_argument("invalid native pub-sub topic");
        for (unsigned char ch : name) {
            if (ch < 0x20 || ch == 0x7f)
                throw std::invalid_argument(
                    "native pub-sub topic contains control character");
        }
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

    void decrement_actor_subscription_locked(const ActorKey& actor) {
        auto count = actor_subscription_counts_.find(actor);
        if (count == actor_subscription_counts_.end()) return;
        if (count->second <= 1) actor_subscription_counts_.erase(count);
        else --count->second;
    }

    bool erase_subscription_locked(
            TopicRecord& topic,
            const ActorKey& actor) {
        auto subscriber = topic.subscribers.find(actor);
        if (subscriber == topic.subscribers.end()) return false;
        queued_payload_bytes_ -= subscriber->second.payload_bytes;
        allocated_queue_slots_ -= subscriber->second.slots.size();
        topic.subscribers.erase(subscriber);
        decrement_actor_subscription_locked(actor);
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
            queued_payload_bytes_ -= it->second.payload_bytes;
            allocated_queue_slots_ -= it->second.slots.size();
            it = topic.subscribers.erase(it);
            decrement_actor_subscription_locked(stale);
        }
    }

    PublishReceipt publish_locked(
            TopicRecord& topic,
            std::shared_ptr<const T> frozen,
            std::size_t payload_bytes) {
        PublishReceipt receipt;
        receipt.subscribers = topic.subscribers.size();

        if (topic.policy == DeliveryPolicy::Reliable) {
            if (payload_bytes != 0
                    && receipt.subscribers
                        > (max_total_bytes_ - queued_payload_bytes_)
                            / payload_bytes) {
                receipt.backpressured = true;
                return receipt;
            }
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

            for (auto& subscriber : topic.subscribers) {
                subscriber.second.push(frozen, payload_bytes);
                queued_payload_bytes_ += payload_bytes;
                ++receipt.accepted;
            }
            return receipt;
        }

        if (topic.policy == DeliveryPolicy::Latest) {
            /*
             * LATEST promises the newest value, so byte-budget failure is an
             * explicit resource rejection, never a silent drop of the new
             * publication. Compute the complete post-replacement graph budget
             * before modifying any queue.
             */
            std::size_t projected = queued_payload_bytes_;
            for (const auto& subscriber : topic.subscribers) {
                const Queue& queue = subscriber.second;
                const std::size_t old =
                    queue.empty() ? 0 : queue.front().bytes;
                projected -= old;
                if (would_exceed(projected, payload_bytes, max_total_bytes_))
                    throw std::logic_error(
                        "LATEST publication exceeds graph byte quota");
                projected += payload_bytes;
            }

            for (auto& subscriber : topic.subscribers) {
                Queue& queue = subscriber.second;
                const bool had_value = !queue.empty();
                const std::size_t old =
                    queue.replace_latest(frozen, payload_bytes);
                queued_payload_bytes_ -= old;
                queued_payload_bytes_ += payload_bytes;
                if (had_value) ++receipt.coalesced;
                ++receipt.accepted;
            }
            return receipt;
        }

        for (auto& subscriber : topic.subscribers) {
            Queue& queue = subscriber.second;
            if (queue.full()
                    || would_exceed(
                        queue.payload_bytes,
                        payload_bytes,
                        max_bytes_per_subscription_)
                    || would_exceed(
                        queued_payload_bytes_,
                        payload_bytes,
                        max_total_bytes_)) {
                ++receipt.dropped;
                continue;
            }
            queue.push(frozen, payload_bytes);
            queued_payload_bytes_ += payload_bytes;
            ++receipt.accepted;
        }
        return receipt;
    }

    const NativeActorMembershipRegistry& memberships_;
    const std::size_t max_topics_;
    const std::size_t max_subscriptions_per_actor_;
    const std::size_t max_bytes_per_subscription_;
    const std::size_t max_total_bytes_;
    const std::size_t max_queue_capacity_;
    const std::size_t max_total_queue_slots_;
    mutable std::mutex mutex_;
    std::map<TopicKey, TopicRecord> topics_;
    std::map<ActorKey, std::size_t> actor_subscription_counts_;
    std::size_t queued_payload_bytes_ = 0;
    std::size_t allocated_queue_slots_ = 0;
};

} // namespace oreslang::runtime
