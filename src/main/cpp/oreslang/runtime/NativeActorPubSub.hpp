#pragma once

#include "oreslang/runtime/NativeActorMembershipRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Host-side bounded admission substrate for ActorGroup pub/sub.
 *
 * This is deliberately not the source-language lowering. Source Topic<T> /
 * Subscription<T> stays fail-closed until the compiler can bind actor
 * continuations and cancellation to this admission layer.
 *
 * RELIABLE publication is atomic at the queue-admission boundary: if any live
 * subscriber queue is full, no subscriber is modified. A scheduler-owned
 * source Future may retry/await capacity later without parking a carrier.
 */
template<class T>
class NativeActorPubSubAdmission final {
public:
    enum class DeliveryPolicy { Reliable, Latest, Lossy };

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
            std::size_t max_subscriptions_per_actor)
        : memberships_(memberships),
          max_topics_(max_topics),
          max_subscriptions_per_actor_(max_subscriptions_per_actor) {
        if (max_topics_ == 0 || max_subscriptions_per_actor_ == 0)
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
            topic.subscribers.emplace(actor, Queue{});
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
        ActorKey actor{actor_id, generation};
        if (!topic->second.subscribers.erase(actor)) return false;
        decrement_actor_subscription_locked(actor);
        return true;
    }

    PublishReceipt publish(
            std::uint64_t group_id,
            const std::string& name,
            std::uint64_t publisher_id,
            std::uint64_t publisher_generation,
            const T& value) {
        return memberships_.with_locked_view([&](const auto& memberships) {
            if (!memberships.contains(
                    group_id, publisher_id, publisher_generation))
                throw std::logic_error(
                        "publication requires live ActorGroup membership");
            std::lock_guard<std::mutex> guard(mutex_);
            TopicRecord& topic = require_topic_locked(group_id, name);
            prune_stale_locked(group_id, topic, memberships);
            return publish_locked(topic, value);
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
            if (subscriber->second.values.empty()) return std::nullopt;
            T value = std::move(subscriber->second.values.front());
            subscriber->second.values.pop_front();
            return value;
        });
    }

    void remove_actor(std::uint64_t actor_id, std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        ActorKey actor{actor_id, generation};
        for (auto& entry : topics_) {
            if (entry.second.subscribers.erase(actor))
                decrement_actor_subscription_locked(actor);
        }
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
            for (const auto& subscriber : it->second.subscribers)
                decrement_actor_subscription_locked(subscriber.first);
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

    struct Queue {
        std::deque<T> values;
    };

    struct TopicRecord {
        DeliveryPolicy policy;
        std::size_t capacity;
        std::map<ActorKey, Queue> subscribers;
    };

    static void validate_topic(
            std::uint64_t group_id,
            const std::string& name,
            DeliveryPolicy policy,
            std::size_t capacity) {
        if (group_id == 0 || name.empty() || capacity == 0)
            throw std::invalid_argument("invalid native pub-sub topic");
        if (policy == DeliveryPolicy::Latest && capacity != 1)
            throw std::invalid_argument("LATEST native pub-sub topic capacity must be one");
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
            ActorKey stale = it->first;
            it = topic.subscribers.erase(it);
            decrement_actor_subscription_locked(stale);
        }
    }

    PublishReceipt publish_locked(
            TopicRecord& topic,
            const T& value) {
        PublishReceipt receipt;
        receipt.subscribers = topic.subscribers.size();

        if (topic.policy == DeliveryPolicy::Reliable) {
            for (const auto& subscriber : topic.subscribers) {
                if (subscriber.second.values.size() >= topic.capacity) {
                    receipt.backpressured = true;
                    return receipt;
                }
            }
            for (auto& subscriber : topic.subscribers) {
                subscriber.second.values.push_back(value);
                ++receipt.accepted;
            }
            return receipt;
        }

        for (auto& subscriber : topic.subscribers) {
            Queue& queue = subscriber.second;
            if (topic.policy == DeliveryPolicy::Latest) {
                if (queue.values.empty()) {
                    queue.values.push_back(value);
                } else {
                    queue.values.back() = value;
                    ++receipt.coalesced;
                }
                ++receipt.accepted;
                continue;
            }

            if (queue.values.size() >= topic.capacity) {
                ++receipt.dropped;
            } else {
                queue.values.push_back(value);
                ++receipt.accepted;
            }
        }
        return receipt;
    }

    const NativeActorMembershipRegistry& memberships_;
    const std::size_t max_topics_;
    const std::size_t max_subscriptions_per_actor_;
    std::mutex mutex_;
    std::map<TopicKey, TopicRecord> topics_;
    std::map<ActorKey, std::size_t> actor_subscription_counts_;
};

} // namespace oreslang::runtime
