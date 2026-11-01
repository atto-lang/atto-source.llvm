#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Trusted host-only immutable TCP slab.
 * A snapshot owns its bytes; pins carry lifetime, not a raw guest pointer.
 * No actor mailbox, foreign pointer, or physical socket is exposed.
 * This mutex-based reference implementation is NOT a hard-real-time path.
 */
class FrozenTcpSlab final {
    struct State {
        explicit State(const std::vector<std::uint8_t>& input) : bytes(input) {}
        const std::vector<std::uint8_t> bytes;
        mutable std::mutex mutex;
        std::size_t pins = 0;
        bool retiring = false;
    };
public:
    static constexpr std::size_t max_bytes = 1024 * 1024;
    class Pin final {
        friend class FrozenTcpSlab;
        explicit Pin(std::shared_ptr<State> state) : state_(std::move(state)), active_(true) {}
        std::shared_ptr<State> state_;
        bool active_ = false;
    public:
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        Pin(Pin&& other) noexcept
            : state_(std::move(other.state_)), active_(std::exchange(other.active_, false)) {}
        Pin& operator=(Pin&&) = delete;
        ~Pin() noexcept {
            if (!active_ || !state_) return;
            std::lock_guard<std::mutex> lock(state_->mutex);
            --state_->pins;
            active_ = false;
        }
    };
    explicit FrozenTcpSlab(const std::vector<std::uint8_t>& input) {
        if (input.size() > max_bytes) throw std::length_error("TCP slab exceeds one MiB");
        state_ = std::make_shared<State>(input);
    }
    FrozenTcpSlab(const FrozenTcpSlab&) = delete;
    FrozenTcpSlab& operator=(const FrozenTcpSlab&) = delete;
    ~FrozenTcpSlab() noexcept { retire(); }

    std::size_t size() const noexcept { return state_->bytes.size(); }
    std::optional<Pin> try_pin() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->retiring || state_->pins == SIZE_MAX) return std::nullopt;
        ++state_->pins;
        return Pin(state_);
    }
    std::optional<std::uint8_t> read_byte(const Pin& pin, std::size_t index) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!pin.active_ || pin.state_.get() != state_.get()
            || index >= state_->bytes.size()) return std::nullopt;
        return state_->bytes[index];
    }
    bool release_pin(Pin& pin) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!pin.active_ || pin.state_.get() != state_.get()) return false;
        pin.active_ = false;
        --state_->pins;
        return true;
    }
    void retire() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->retiring = true;
    }
    bool freed() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->retiring && state_->pins == 0;
    }
    std::size_t pin_count() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->pins;
    }
private:
    std::shared_ptr<State> state_;
};
} // namespace oreslang::runtime
