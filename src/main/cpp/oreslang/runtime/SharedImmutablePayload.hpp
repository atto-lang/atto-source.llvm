#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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

    // Trusted host I/O only: callback must NOT store the borrowed span or pass
    // it to an asynchronous syscall without separately retaining a descriptor.
    // No raw address is ever placed in a guest mailbox.
    template<class Fn>
    decltype(auto) with_read_only_span(Fn&& fn) const {
        if (!backing_) throw std::logic_error("invalid shared payload");
        const auto* bytes = backing_->bytes.data();
        if (offset_ != 0) bytes += offset_;
        return std::forward<Fn>(fn)(std::span<const std::uint8_t>(bytes, length_));
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
    SharedPayloadInbox(std::size_t max_messages, std::size_t max_charged_bytes)
        : max_messages_(max_messages), max_bytes_(max_charged_bytes) {
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
        if (queue_.size() >= max_messages_ || charge > max_bytes_ - charged_)
            return SendResult::full;
        queue_.push_back(descriptor); // May throw; no credits committed yet.
        charged_ += charge;
        return SendResult::admitted;
    }

    std::optional<SharedImmutablePayload> try_receive() {
        std::unique_lock lock(mu_, std::try_to_lock);
        if (!lock.owns_lock() || queue_.empty()) return std::nullopt;
        SharedImmutablePayload item = std::move(queue_.front());
        charged_ -= item.charged_bytes();
        queue_.pop_front();
        return item;
    }

    bool try_close() {
        std::unique_lock lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        closed_ = true;
        queue_.clear(); // Releases queued handles, never an active receiver.
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
        return queue_.size();
    }
private:
    mutable std::mutex mu_;
    std::deque<SharedImmutablePayload> queue_;
    const std::size_t max_messages_;
    const std::size_t max_bytes_;
    std::size_t charged_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::runtime
