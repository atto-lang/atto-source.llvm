#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Trusted native actor-transport substrate, not a guest-visible pointer ABI.
// Publication moves an exclusively owned allocation; readers hold immutable,
// ref-counted descriptors. The trusted caller MUST NOT keep mutable aliases to
// the moved allocation; only source-level ownership checking can prove that.
// A slice retains (and is charged for) the WHOLE slab.
class SharedImmutablePayload final {
    struct Backing final {
        explicit Backing(std::vector<std::uint8_t>&& input)
            : bytes(std::move(input)) {}
        const std::vector<std::uint8_t> bytes;
    };
    std::shared_ptr<const Backing> backing_;
    std::size_t offset_ = 0;
    std::size_t length_ = 0;

    SharedImmutablePayload(std::shared_ptr<const Backing> backing,
                           std::size_t offset, std::size_t length) noexcept
        : backing_(std::move(backing)), offset_(offset), length_(length) {}
public:
    static constexpr std::size_t max_slab_bytes = 16 * 1024 * 1024;
    SharedImmutablePayload() = default;

    static SharedImmutablePayload publish(std::vector<std::uint8_t>&& owned) {
        if (owned.size() > max_slab_bytes)
            throw std::length_error("shared payload exceeds slab limit");
        auto backing = std::make_shared<const Backing>(std::move(owned));
        const auto length = backing->bytes.size();
        return SharedImmutablePayload(std::move(backing), 0, length);
    }

    bool valid() const noexcept { return static_cast<bool>(backing_); }
    std::size_t size() const noexcept { return length_; }
    std::size_t charged_bytes() const noexcept {
        return backing_ ? backing_->bytes.size() : 0;
    }

    std::optional<SharedImmutablePayload> slice(
            std::size_t offset, std::size_t length) const noexcept {
        if (!backing_ || offset > length_ || length > length_ - offset)
            return std::nullopt;
        return SharedImmutablePayload(backing_, offset_ + offset, length);
    }

    std::optional<std::uint8_t> read_byte(std::size_t i) const noexcept {
        if (!backing_ || i >= length_) return std::nullopt;
        return backing_->bytes[offset_ + i];
    }

    // Trusted host I/O only: callback is synchronous and cannot return a
    // borrowed view. Host callbacks must not capture/store the span elsewhere.
    // Async I/O must retain a descriptor until its physical completion.
    template<class Fn>
        requires std::is_void_v<std::invoke_result_t<Fn,
                               std::span<const std::uint8_t>>>
    void with_read_only_span(Fn&& fn) const {
        static_assert(std::is_void_v<std::invoke_result_t<Fn,
                           std::span<const std::uint8_t>>>,
                      "read-only payload callbacks must return void; borrowed views cannot escape");
        if (!backing_) throw std::logic_error("invalid shared payload");
        const auto* bytes = backing_->bytes.data();
        if (offset_ != 0) bytes += offset_;
        std::forward<Fn>(fn)(std::span<const std::uint8_t>(bytes, length_));
    }

    // Host-only verification of pointer identity (not serialized/guest-visible).
    bool same_backing(const SharedImmutablePayload& other) const noexcept {
        return backing_ && backing_.get() == other.backing_.get();
    }
};

// Bounded mailbox DATA lane for immutable local payload descriptors. No
// waiting on actor readiness: try_lock returns BUSY rather than parking.
// Does not replace supervisor CONTROL delivery or compile guest actor sends.
class SharedPayloadInbox final {
public:
    enum class SendResult { admitted, busy, full, closed, invalid };
    enum class ReceiveStatus { received, busy, empty, closed };
    struct ReceiveResult final {
        ReceiveStatus status;
        std::optional<SharedImmutablePayload> descriptor;
    };

    // Allocate slot metadata at construction, NEVER on the send path.
    // Actual payload backing is published separately before admission.
    SharedPayloadInbox(std::size_t max_messages, std::size_t max_charged_bytes)
        : ring_(max_messages), max_messages_(max_messages),
          max_bytes_(max_charged_bytes) {
        if (!max_messages_) throw std::invalid_argument("zero mailbox slots");
    }
    SharedPayloadInbox(const SharedPayloadInbox&) = delete;
    SharedPayloadInbox& operator=(const SharedPayloadInbox&) = delete;

    SendResult try_send(const SharedImmutablePayload& descriptor) {
        if (!descriptor.valid()) return SendResult::invalid;
        std::unique_lock lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return SendResult::busy;
        if (closed_) return SendResult::closed;
        const auto charge = descriptor.charged_bytes();
        if (queued_ >= max_messages_ || charge > max_bytes_ - charged_)
            return SendResult::full;
        // Ring slot is empty by construction or after a prior dequeue.
        // Copying a shared descriptor does not allocate or copy payload bytes.
        static_assert(std::is_nothrow_copy_constructible_v<SharedImmutablePayload>);
        ring_[tail_].emplace(descriptor);
        tail_ = tail_ + 1 == max_messages_ ? 0 : tail_ + 1;
        ++queued_;
        charged_ += charge;
        return SendResult::admitted;
    }

    ReceiveResult try_receive_result() {
        std::unique_lock lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return {ReceiveStatus::busy, std::nullopt};
        if (!queued_)
            return {closed_ ? ReceiveStatus::closed : ReceiveStatus::empty,
                    std::nullopt};
        auto item = std::move(*ring_[head_]);
        ring_[head_].reset();
        head_ = head_ + 1 == max_messages_ ? 0 : head_ + 1;
        --queued_;
        charged_ -= item.charged_bytes();
        return {ReceiveStatus::received, std::move(item)};
    }

    // Compatibility accessor. Schedulers must use try_receive_result() so
    // BUSY, EMPTY and CLOSED never become indistinguishable wakeup states.
    std::optional<SharedImmutablePayload> try_receive() {
        return std::move(try_receive_result().descriptor);
    }

    bool try_close() {
        std::unique_lock lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        closed_ = true;
        // External STOP/quiescence is required. Final reference release may
        // reclaim backing memory and is not a hard real-time actor operation.
        for (auto& entry : ring_) entry.reset();
        head_ = 0;
        tail_ = 0;
        queued_ = 0;
        charged_ = 0;
        return true;
    }

    // Trusted host diagnostics only: blocking mutex acquisition here is not
    // permitted on a hard-real-time actor thread.
    std::size_t charged_bytes() const {
        std::lock_guard lock(mu_);
        return charged_;
    }
    std::size_t queued() const {
        std::lock_guard lock(mu_);
        return queued_;
    }
private:
    mutable std::mutex mu_;
    std::vector<std::optional<SharedImmutablePayload>> ring_;
    const std::size_t max_messages_;
    const std::size_t max_bytes_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t queued_ = 0;
    std::size_t charged_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::runtime
