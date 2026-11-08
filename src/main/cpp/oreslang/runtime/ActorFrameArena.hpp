#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace oreslang::runtime {

namespace detail {
// One physical carrier may enter one actor frame arena, even when templates
// have different capacity specializations.
inline thread_local const void* active_actor_frame_arena = nullptr;
} // namespace detail

// Native, bounded per-actor *logical* frame bytes. This is intentionally NOT an
// OS stack, a compiler-lowered frame layout, or a native-fault containment
// boundary. Only a trusted runtime adapter should enter/exit physical turns.
// Each actor incarnation owns a distinct instance; this is not task-global TLS.
template<std::size_t MaxFrames = 64, std::size_t MaxBytes = 64 * 1024>
class ActorFrameArena final {
    static_assert(MaxFrames > 0 && MaxBytes > 0, "actor frame quotas must be positive");
public:
    class FrameRef final {
        friend class ActorFrameArena;
        std::uint64_t arena_ = 0;
        std::uint64_t execution_ = 0;
        std::uint64_t sequence_ = 0;
        std::size_t index_ = 0;
        FrameRef(std::uint64_t a, std::uint64_t e,
                 std::uint64_t s, std::size_t i) noexcept
            : arena_(a), execution_(e), sequence_(s), index_(i) {}
    public:
        FrameRef() = default;
    };

private:
    struct Frame {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::uint64_t sequence = 0;
        std::uint64_t execution = 0;
    };
    alignas(std::max_align_t) std::array<std::byte, MaxBytes> data_{};
    std::array<Frame, MaxFrames> frames_{};
    std::atomic<bool> occupied_{false};
    std::atomic<bool> poisoned_{false};
    std::atomic<bool> retired_{false};
    inline static std::atomic<std::uint64_t> next_arena_id_{1};
    const std::uint64_t arena_id_;
    std::uint64_t execution_ = 0;
    std::uint64_t suspended_execution_ = 0;
    std::uint64_t next_frame_sequence_ = 1;
    std::size_t count_ = 0;
    std::size_t bytes_ = 0;

    static std::uint64_t reserve_identity() noexcept {
        auto value = next_arena_id_.load(std::memory_order_relaxed);
        while (value != 0 && value != std::numeric_limits<std::uint64_t>::max()) {
            if (next_arena_id_.compare_exchange_weak(value, value + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return value;
        }
        return 0;  // exhausted: refuse all turns; never wrap and reuse a handle
    }

    static void wipe(std::byte* start, std::size_t length) noexcept {
        volatile std::byte* destination = start;
        for (std::size_t i = 0; i < length; ++i)
            destination[i] = std::byte{0};
    }

    bool owns_turn() const noexcept {
        return detail::active_actor_frame_arena == this && execution_ != 0;
    }

    Frame* find(FrameRef ref) noexcept {
        if (!owns_turn() || ref.arena_ != arena_id_ ||
            ref.execution_ != execution_ || ref.sequence_ == 0 ||
            ref.index_ >= count_ || poisoned_.load(std::memory_order_relaxed))
            return nullptr;
        Frame& frame = frames_[ref.index_];
        return frame.sequence == ref.sequence_ &&
               frame.execution == ref.execution_ ? &frame : nullptr;
    }

    void release_turn() noexcept {
        execution_ = 0;
        detail::active_actor_frame_arena = nullptr;
        occupied_.store(false, std::memory_order_release);
    }

public:
    ActorFrameArena() noexcept : arena_id_(reserve_identity()) {}
    ~ActorFrameArena() noexcept { wipe(data_.data(), data_.size()); }
    ActorFrameArena(const ActorFrameArena&) = delete;
    ActorFrameArena& operator=(const ActorFrameArena&) = delete;

    // Nonparking admission: a suspended logical execution may resume on a
    // different carrier, but no other execution can borrow its live frames.
    bool enter(std::uint64_t execution) noexcept {
        if (!execution || !arena_id_ || detail::active_actor_frame_arena) return false;
        bool expected = false;
        if (!occupied_.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed))
            return false;
        if (retired_.load(std::memory_order_relaxed) ||
            poisoned_.load(std::memory_order_relaxed) ||
            (suspended_execution_ && suspended_execution_ != execution)) {
            occupied_.store(false, std::memory_order_release);
            return false;
        }
        if (suspended_execution_ == execution) suspended_execution_ = 0;
        execution_ = execution;
        detail::active_actor_frame_arena = this;
        return true;
    }

    std::optional<FrameRef> push(std::size_t bytes) noexcept {
        if (!owns_turn() || poisoned_.load(std::memory_order_relaxed))
            return std::nullopt;
        if (!bytes || count_ == MaxFrames || bytes > MaxBytes - bytes_ ||
            next_frame_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            poisoned_.store(true, std::memory_order_release);
            return std::nullopt;
        }
        const auto index = count_++;
        const auto serial = next_frame_sequence_++;
        frames_[index] = Frame{bytes_, bytes, serial, execution_};
        wipe(data_.data() + bytes_, bytes);
        bytes_ += bytes;
        return FrameRef{arena_id_, execution_, serial, index};
    }

    bool write(FrameRef ref, std::size_t offset,
               const void* input, std::size_t length) noexcept {
        auto* frame = find(ref);
        if (!frame || (!input && length) || offset > frame->length ||
            length > frame->length - offset) return false;
        if (length) std::memcpy(data_.data() + frame->offset + offset, input, length);
        return true;
    }

    bool read(FrameRef ref, std::size_t offset,
              void* output, std::size_t length) noexcept {
        auto* frame = find(ref);
        if (!frame || (!output && length) || offset > frame->length ||
            length > frame->length - offset) return false;
        if (length) std::memcpy(output, data_.data() + frame->offset + offset, length);
        return true;
    }

    // Non-LIFO release is a permanent actor-local protocol failure.
    bool pop(FrameRef ref) noexcept {
        if (!owns_turn()) return false;
        Frame* frame = find(ref);
        if (!frame || ref.index_ + 1 != count_) {
            poisoned_.store(true, std::memory_order_release);
            return false;
        }
        wipe(data_.data() + frame->offset, frame->length);
        bytes_ = frame->offset;
        *frame = Frame{};
        --count_;
        return true;
    }

    // A logical frame is not released on await or cooperation.
    bool suspend() noexcept {
        if (!owns_turn() || poisoned_.load(std::memory_order_acquire)) return false;
        suspended_execution_ = execution_;
        release_turn();
        return true;
    }

    // A completed actor turn may not leak any logical frame into the next turn.
    bool finish() noexcept {
        if (!owns_turn()) return false;
        const bool valid = count_ == 0 && !poisoned_.load(std::memory_order_acquire);
        if (!valid) poisoned_.store(true, std::memory_order_release);
        release_turn();
        return valid;
    }

    // Cancellation is legal on a poisoned turn; it reclaims bytes but does not
    // clear poison or grant a new execution. This is not supervisor restart.
    bool cancel_active() noexcept {
        if (!owns_turn()) return false;
        wipe(data_.data(), data_.size());
        frames_.fill(Frame{});
        count_ = bytes_ = 0;
        suspended_execution_ = 0;
        release_turn();
        return true;
    }

    // Requires supervisor-established STOP + callback/continuation quiescence.
    // CAS refuses racing physical admission; no actor frame can be freed live.
    bool retire_quiesced() noexcept {
        if (detail::active_actor_frame_arena == this) return false;
        bool expected = false;
        if (!occupied_.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed))
            return false;
        wipe(data_.data(), data_.size());
        frames_.fill(Frame{});
        count_ = bytes_ = 0;
        suspended_execution_ = execution_ = 0;
        retired_.store(true, std::memory_order_release);
        occupied_.store(false, std::memory_order_release);
        return true;
    }

    bool poisoned() const noexcept { return poisoned_.load(std::memory_order_acquire); }
    bool retired() const noexcept { return retired_.load(std::memory_order_acquire); }
    std::size_t active_frames() const noexcept { return owns_turn() ? count_ : 0; }
    std::size_t active_bytes() const noexcept { return owns_turn() ? bytes_ : 0; }
};

} // namespace oreslang::runtime
