#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

// Compiler/backend integration substrate, NOT guest codegen or protection
// against an already exhausted host/native stack. The compiler must call
// enter_frame BEFORE each guest activation and leave_frame on all exits.
// A scheduler must read terminal_overflow() after a guest callback, even if
// foreign host code catches Overflow: a terminal actor PANIC is not recoverable.
namespace oreslang {

enum class ActorFrameOverflowLimit : std::uint8_t {
  FrameCount,
  ByteCapacity
};

class ActorCheckedFrameBudget final {
 public:
  class Overflow final : public std::exception {
    ActorFrameOverflowLimit limit_;
   public:
    explicit Overflow(ActorFrameOverflowLimit limit) noexcept : limit_(limit) {}
    ActorFrameOverflowLimit limit() const noexcept { return limit_; }
    const char* what() const noexcept override {
      return limit_ == ActorFrameOverflowLimit::FrameCount
          ? "actor checked frame-count budget exhausted"
          : "actor checked stack byte budget exhausted";
    }
  };
  class FrameToken final {
    friend class ActorCheckedFrameBudget;
    const ActorCheckedFrameBudget* owner_ = nullptr;
    std::uint64_t serial_ = 0;
    std::uint64_t previous_serial_ = 0;
    std::uint32_t depth_ = 0;
    std::size_t before_bytes_ = 0;
    std::size_t after_bytes_ = 0;
    FrameToken(const ActorCheckedFrameBudget* owner, std::uint64_t serial,
               std::uint64_t previous, std::uint32_t depth,
               std::size_t before, std::size_t after) noexcept
        : owner_(owner), serial_(serial), previous_serial_(previous),
          depth_(depth), before_bytes_(before), after_bytes_(after) {}
   public:
    FrameToken() = default;
  };

  ActorCheckedFrameBudget(std::size_t max_bytes, std::uint32_t max_frames)
      : max_bytes_(max_bytes), max_frames_(max_frames) {
    if (max_bytes == 0 || max_frames == 0 || max_frames > 1024)
      throw std::invalid_argument("invalid actor checked frame budget");
  }
  ActorCheckedFrameBudget(const ActorCheckedFrameBudget&) = delete;
  ActorCheckedFrameBudget& operator=(const ActorCheckedFrameBudget&) = delete;
  ActorCheckedFrameBudget(ActorCheckedFrameBudget&&) = delete;
  ActorCheckedFrameBudget& operator=(ActorCheckedFrameBudget&&) = delete;

  FrameToken enter_frame(std::size_t bytes) {
    if (terminal_) throw Overflow(*terminal_);
    if (bytes == 0) throw std::invalid_argument("actor frame bytes must be positive");
    if (depth_ >= max_frames_) fail(ActorFrameOverflowLimit::FrameCount);
    if (bytes > max_bytes_ - used_bytes_)
      fail(ActorFrameOverflowLimit::ByteCapacity);
    // No counter wrap may accidentally make an old token valid again.
    if (issued_ == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("actor frame token generation exhausted");
    const auto previous = top_;
    const auto serial = ++issued_;
    const auto before = used_bytes_;
    used_bytes_ += bytes;
    ++depth_;
    top_ = serial;
    return FrameToken(this, serial, previous, depth_, before, used_bytes_);
  }

  void leave_frame(const FrameToken& token) {
    if (token.owner_ != this || depth_ == 0 || token.depth_ != depth_ ||
        token.serial_ == 0 || token.serial_ != top_ ||
        token.after_bytes_ != used_bytes_ ||
        token.before_bytes_ >= used_bytes_)
      throw std::logic_error("foreign, stale or out-of-order actor frame");
    top_ = token.previous_serial_;
    used_bytes_ = token.before_bytes_;
    --depth_;
    // A terminal PANIC stays latched after cleanup.
  }

  std::optional<ActorFrameOverflowLimit> terminal_overflow() const noexcept {
    return terminal_;
  }
  std::size_t used_bytes() const noexcept { return used_bytes_; }
  std::uint32_t active_frames() const noexcept { return depth_; }

 private:
  [[noreturn]] void fail(ActorFrameOverflowLimit limit) {
    terminal_ = limit;
    throw Overflow(limit);
  }
  const std::size_t max_bytes_;
  const std::uint32_t max_frames_;
  std::size_t used_bytes_ = 0;
  std::uint32_t depth_ = 0;
  std::uint64_t top_ = 0;
  std::uint64_t issued_ = 0;
  std::optional<ActorFrameOverflowLimit> terminal_;
};
} // namespace oreslang
