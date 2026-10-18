#pragma once
#include "oreslang/runtime/PersistentActorMap.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace oreslang::runtime {

enum class WireFormat : std::uint8_t { MessagePackV2, Json, Protobuf };

template <typename T> struct ActorSendable : std::false_type {};
template <> struct ActorSendable<bool> : std::true_type {};
template <> struct ActorSendable<std::int64_t> : std::true_type {};
template <> struct ActorSendable<double> : std::true_type {};
template <> struct ActorSendable<std::string> : std::true_type {};
template <> struct ActorSendable<PersistentActorMap::MapRef> : std::true_type {};
template <typename T>
inline constexpr bool actor_sendable_v = ActorSendable<std::decay_t<T>>::value;

template <WireFormat, typename T> struct WireSerializable : std::false_type {};
template <> struct WireSerializable<WireFormat::MessagePackV2, bool> : std::true_type {};
template <> struct WireSerializable<WireFormat::MessagePackV2, std::int64_t> : std::true_type {};
template <> struct WireSerializable<WireFormat::MessagePackV2, double> : std::true_type {};
template <> struct WireSerializable<WireFormat::MessagePackV2, std::string> : std::true_type {};
template <WireFormat F, typename T>
inline constexpr bool wire_serializable_v =
    WireSerializable<F, std::decay_t<T>>::value;

class ActorRefAuthority final {
    friend class RuntimeAuthorityFactory;
    std::uint64_t actor_, generation_;
    ActorRefAuthority(std::uint64_t actor, std::uint64_t generation)
        : actor_(actor), generation_(generation) {}
public:
    ActorRefAuthority() = delete;
    std::uint64_t actor() const noexcept { return actor_; }
    std::uint64_t generation() const noexcept { return generation_; }
};

class RuntimeAuthorityFactory final {
public:
    static ActorRefAuthority actor_ref(std::uint64_t actor,
                                       std::uint64_t generation) {
        if (!actor || !generation)
            throw std::invalid_argument("invalid actor authority");
        return ActorRefAuthority(actor, generation);
    }
};

enum class ControlKind : std::uint8_t { Panic, Cancel, HostFailure };

class SupervisorControlEvent final {
    friend class SupervisorControlFactory;
    std::uint64_t actor_, generation_;
    ControlKind kind_;
    SupervisorControlEvent(std::uint64_t actor, std::uint64_t generation,
                           ControlKind kind)
        : actor_(actor), generation_(generation), kind_(kind) {}
public:
    SupervisorControlEvent() = delete;
    std::uint64_t actor() const noexcept { return actor_; }
    std::uint64_t generation() const noexcept { return generation_; }
    ControlKind kind() const noexcept { return kind_; }
};

class SupervisorControlFactory final {
public:
    static SupervisorControlEvent terminal(std::uint64_t actor,
                                           std::uint64_t generation,
                                           ControlKind kind) {
        if (!actor || !generation)
            throw std::invalid_argument("invalid CONTROL target");
        return SupervisorControlEvent(actor, generation, kind);
    }
};

using ActorDataValue = std::variant<
    bool, std::int64_t, double, std::string, PersistentActorMap::MapRef>;

class ActorDataLane final {
    struct Frame {
        std::uint64_t id;
        std::size_t bytes;
        ActorDataValue value;
    };
    struct State {
        explicit State(std::size_t frames, std::size_t bytes)
            : max_frames(frames), max_bytes(bytes) {
            if (!frames || !bytes)
                throw std::invalid_argument("positive DATA limits required");
        }
        std::mutex lock;
        std::deque<Frame> queue;
        std::size_t max_frames, max_bytes;
        std::size_t used_frames = 0, used_bytes = 0;
        std::uint64_t next_id = 1;
        bool closed = false;
    };

public:
    class Delivery final {
    public:
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&&) = default;
        Delivery& operator=(Delivery&&) = default;

        const ActorDataValue& value() const {
            if (acked_) throw std::logic_error("stale DATA delivery");
            return value_;
        }

        bool acknowledge() {
            if (acked_ || !state_) return false;
            std::lock_guard<std::mutex> guard(state_->lock);
            if (!state_->used_frames || state_->used_bytes < bytes_)
                throw std::logic_error("DATA credit accounting corrupted");
            --state_->used_frames;
            state_->used_bytes -= bytes_;
            acked_ = true;
            return true;
        }

    private:
        friend class ActorDataLane;
        Delivery(std::shared_ptr<State> state, Frame frame)
            : state_(std::move(state)), bytes_(frame.bytes),
              value_(std::move(frame.value)) {}
        std::shared_ptr<State> state_;
        std::size_t bytes_;
        ActorDataValue value_;
        bool acked_ = false;
    };

    ActorDataLane(std::size_t frames, std::size_t bytes)
        : state_(std::make_shared<State>(frames, bytes)) {}

    bool offer(ActorDataValue value, std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("DATA byte charge required");
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->closed) throw std::logic_error("DATA lane closed");
        if (state_->next_id == 0)
            throw std::overflow_error("DATA identity exhausted");
        if (state_->used_frames >= state_->max_frames ||
            bytes > state_->max_bytes - state_->used_bytes)
            return false;
        state_->queue.push_back(
            Frame{state_->next_id++, bytes, std::move(value)});
        ++state_->used_frames;
        state_->used_bytes += bytes;
        return true;
    }

    std::optional<Delivery> try_receive() {
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->queue.empty()) return std::nullopt;
        Frame frame = std::move(state_->queue.front());
        state_->queue.pop_front();
        return std::optional<Delivery>(Delivery(state_, std::move(frame)));
    }

    std::size_t used_frames() const {
        std::lock_guard<std::mutex> guard(state_->lock);
        return state_->used_frames;
    }

private:
    std::shared_ptr<State> state_;
};

class SupervisorControlLane final {
public:
    explicit SupervisorControlLane(std::size_t capacity)
        : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("CONTROL capacity required");
    }

    bool publish(SupervisorControlEvent event) {
        std::lock_guard<std::mutex> guard(lock_);
        if (queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(event));
        return true;
    }

    std::optional<SupervisorControlEvent> poll() {
        std::lock_guard<std::mutex> guard(lock_);
        if (queue_.empty()) return std::nullopt;
        auto event = std::move(queue_.front());
        queue_.pop_front();
        return event;
    }

private:
    std::size_t capacity_;
    std::mutex lock_;
    std::deque<SupervisorControlEvent> queue_;
};

} // namespace oreslang::runtime
