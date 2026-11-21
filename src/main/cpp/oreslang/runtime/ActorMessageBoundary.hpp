#pragma once
#include "oreslang/runtime/PersistentActorMap.hpp"
#include <cstddef>
#include <cmath>
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
    SupervisorControlEvent(const SupervisorControlEvent&) = delete;
    SupervisorControlEvent& operator=(const SupervisorControlEvent&) = delete;
    SupervisorControlEvent(SupervisorControlEvent&& other) noexcept
        : actor_(other.actor_), generation_(other.generation_), kind_(other.kind_) {
        other.actor_ = 0;
        other.generation_ = 0;
    }
    SupervisorControlEvent& operator=(SupervisorControlEvent&& other) noexcept {
        if (this != &other) {
            actor_ = other.actor_;
            generation_ = other.generation_;
            kind_ = other.kind_;
            other.actor_ = 0;
            other.generation_ = 0;
        }
        return *this;
    }
    std::uint64_t actor() const noexcept { return actor_; }
    std::uint64_t generation() const noexcept { return generation_; }
    ControlKind kind() const noexcept { return kind_; }
    bool valid() const noexcept { return actor_ != 0 && generation_ != 0; }
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

inline void validateActorData(const ActorDataValue& value) {
    if (auto number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number))
            throw std::invalid_argument("nonfinite DATA float");
        return;
    }
    if (auto text = std::get_if<std::string>(&value)) {
        const auto& s = *text;
        for (std::size_t i = 0; i < s.size();) {
            const auto a = static_cast<unsigned char>(s[i]);
            if (a <= 0x7f) { ++i; continue; }
            int trailing = 0;
            if (a >= 0xc2 && a <= 0xdf) trailing = 1;
            else if (a >= 0xe0 && a <= 0xef) trailing = 2;
            else if (a >= 0xf0 && a <= 0xf4) trailing = 3;
            else throw std::invalid_argument("invalid UTF-8 DATA string");
            if (static_cast<std::size_t>(trailing) > s.size() - i - 1)
                throw std::invalid_argument("truncated UTF-8 DATA string");
            const auto b = static_cast<unsigned char>(s[i + 1]);
            if (b < 0x80 || b > 0xbf ||
                (a == 0xe0 && b < 0xa0) || (a == 0xed && b > 0x9f) ||
                (a == 0xf0 && b < 0x90) || (a == 0xf4 && b > 0x8f))
                throw std::invalid_argument("invalid UTF-8 DATA string");
            for (int j = 2; j <= trailing; ++j) {
                const auto c = static_cast<unsigned char>(s[i + j]);
                if (c < 0x80 || c > 0xbf)
                    throw std::invalid_argument("invalid UTF-8 DATA string");
            }
            i += static_cast<std::size_t>(trailing + 1);
        }
        return;
    }
    if (auto root = std::get_if<PersistentActorMap::MapRef>(&value);
        root && !*root)
        throw std::invalid_argument("null persistent DATA root");
}

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

    bool offer(const ActorDataValue& value, std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("DATA byte charge required");
        validateActorData(value);
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->closed) throw std::logic_error("DATA lane closed");
        if (state_->next_id == 0)
            throw std::overflow_error("DATA identity exhausted");
        if (state_->used_frames >= state_->max_frames ||
            bytes > state_->max_bytes - state_->used_bytes)
            return false;
        state_->queue.push_back(
            Frame{state_->next_id++, bytes, value});
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

    void close() {
        std::lock_guard<std::mutex> guard(state_->lock);
        state_->closed = true;
        if (state_->used_frames != 0)
            throw std::logic_error("DATA lane has unacknowledged deliveries");
    }

private:
    std::shared_ptr<State> state_;
};

class SupervisorControlLane final {
public:
    SupervisorControlLane(std::uint64_t actor, std::uint64_t generation,
                          std::size_t capacity)
        : actor_(actor), generation_(generation), capacity_(capacity) {
        if (!actor_ || !generation_ || !capacity_)
            throw std::invalid_argument("CONTROL target/capacity required");
    }

    bool publish(SupervisorControlEvent&& event) {
        std::lock_guard<std::mutex> guard(lock_);
        if (!event.valid() || event.actor() != actor_ ||
            event.generation() != generation_)
            throw std::logic_error("stale/foreign CONTROL authority");
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
    std::uint64_t actor_, generation_;
    std::size_t capacity_;
    std::mutex lock_;
    std::deque<SupervisorControlEvent> queue_;
};

} // namespace oreslang::runtime
