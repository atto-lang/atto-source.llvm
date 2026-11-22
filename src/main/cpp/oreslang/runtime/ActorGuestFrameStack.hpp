#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

// Bounded, actor-owned LOGICAL guest frame bytes. This is not an OS stack,
// stack switcher, memory sandbox, or a replacement for compiler stack guards.
// All runtime methods are invoked solely through an authenticated carrier turn.
class ActorGuestFrameStack final {
public:
    static constexpr std::size_t capacity_bytes = 4096;
    static constexpr std::size_t max_depth = 32;
    static constexpr std::size_t max_frame_bytes = 256;
    struct FrameToken {
        std::uint64_t actor_generation = 0;
        std::uint64_t serial = 0;
        std::uint32_t depth = 0;
    };

    ActorGuestFrameStack() = default;
    ActorGuestFrameStack(const ActorGuestFrameStack&) = delete;
    ActorGuestFrameStack& operator=(const ActorGuestFrameStack&) = delete;
    ~ActorGuestFrameStack() noexcept { scrub_all(); }

    bool initialize(std::uint64_t generation) noexcept {
        if (generation == 0 || generation_ != 0 || !closed_ || ever_initialized_) return false;
        generation_ = generation;
        ever_initialized_ = true;
        closed_ = false;
        serial_ = 0;
        return true;
    }
    bool push(std::size_t bytes, FrameToken& out) noexcept {
        out = {};
        if (closed_ || generation_ == 0 || bytes == 0 ||
                bytes > max_frame_bytes || depth_ >= max_depth ||
                bytes > capacity_bytes - used_ || serial_ == UINT64_MAX)
            return false;
        auto& frame = frames_[depth_];
        frame.offset = used_;
        frame.size = static_cast<std::uint32_t>(bytes);
        frame.serial = ++serial_;
        ++depth_;
        used_ += bytes;
        out = {generation_, frame.serial, static_cast<std::uint32_t>(depth_)};
        return true;
    }
    bool write(FrameToken token, std::size_t offset,
               const void* source, std::size_t bytes) noexcept {
        if (!source || !valid_top(token, offset, bytes)) return false;
        const auto base = frames_[depth_ - 1].offset + offset;
        const auto* src = static_cast<const unsigned char*>(source);
        for (std::size_t i = 0; i < bytes; ++i) storage_[base + i] = src[i];
        return true;
    }
    bool read(FrameToken token, std::size_t offset,
              void* destination, std::size_t bytes) const noexcept {
        if (!destination || !valid_top(token, offset, bytes)) return false;
        const auto base = frames_[depth_ - 1].offset + offset;
        auto* dst = static_cast<unsigned char*>(destination);
        for (std::size_t i = 0; i < bytes; ++i) dst[i] = storage_[base + i];
        return true;
    }
    bool pop(FrameToken token) noexcept {
        if (!valid_top(token, 0, 0)) return false;
        const auto frame = frames_[depth_ - 1];
        scrub(frame.offset, frame.size);
        used_ = frame.offset;
        frames_[depth_ - 1] = {};
        --depth_;
        return true;
    }
    std::size_t depth() const noexcept { return depth_; }
    std::size_t used_bytes() const noexcept { return used_; }
    void clear_turn() noexcept {
        scrub_all();
        used_ = 0;
        depth_ = 0;
        for (auto& frame : frames_) frame = {};
        // Serial remains monotonic across messages on the same incarnation.
    }
    void retire() noexcept {
        clear_turn();
        generation_ = 0;
        closed_ = true;
    }
private:
    struct Frame {
        std::size_t offset = 0;
        std::uint32_t size = 0;
        std::uint64_t serial = 0;
    };
    bool valid_top(FrameToken token, std::size_t offset,
                   std::size_t bytes) const noexcept {
        if (closed_ || generation_ == 0 || token.actor_generation != generation_ ||
                depth_ == 0 || token.depth != depth_) return false;
        const auto& frame = frames_[depth_ - 1];
        return token.serial != 0 && token.serial == frame.serial &&
               offset <= frame.size && bytes <= frame.size - offset;
    }
    void scrub(std::size_t offset, std::size_t count) noexcept {
        // Volatile byte writes prevent an optimizing compiler removing scrubs
        // immediately before reclamation. No guest-accessible backing pointer.
        volatile unsigned char* bytes = storage_.data();
        for (std::size_t i = 0; i < count; ++i) bytes[offset + i] = 0;
    }
    void scrub_all() noexcept { scrub(0, capacity_bytes); }

    std::array<unsigned char, capacity_bytes> storage_{};
    std::array<Frame, max_depth> frames_{};
    std::size_t used_ = 0, depth_ = 0;
    std::uint64_t generation_ = 0, serial_ = 0;
    bool closed_ = true, ever_initialized_ = false;
};

} // namespace oreslang::runtime
