#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace oreslang::runtime {

/**
 * Fixed-capacity compiler cleanup scope for ActorOwnedHeap-like heaps.
 *
 * This is deliberately allocation-free: the scope stores tracked handles in
 * inline storage and releases them in reverse allocation order. It models the
 * lowering the borrow checker needs for lexical owners: successful allocation
 * arms cleanup, move/ownership transfer disarms it, and scope exit drops every
 * still-owned allocation exactly once.
 *
 * Heap must expose Handle, allocate(count, alignment) and remove(handle).
 */
template<class Heap, std::size_t MaxTracked>
class ActorHeapScope final {
    static_assert(MaxTracked > 0, "positive cleanup scope capacity required");

public:
    using Handle = typename Heap::Handle;

    explicit ActorHeapScope(Heap& heap) noexcept : heap_(&heap) {}
    ActorHeapScope(const ActorHeapScope&) = delete;
    ActorHeapScope& operator=(const ActorHeapScope&) = delete;
    ActorHeapScope(ActorHeapScope&&) = delete;
    ActorHeapScope& operator=(ActorHeapScope&&) = delete;

    ~ActorHeapScope() noexcept { (void)close(); }

    std::optional<Handle> allocate(
            std::size_t count,
            std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (closed_ || tracked_ == MaxTracked) return std::nullopt;
        auto handle = heap_->allocate(count, alignment);
        if (!handle) return std::nullopt;
        handles_[tracked_++] = *handle;
        return handle;
    }

    /**
     * Transfer ownership out of this lexical scope without freeing storage.
     * The caller becomes responsible for exactly-once cleanup.
     */
    bool release(const Handle& handle) noexcept {
        if (closed_) return false;
        for (std::size_t i = 0; i < tracked_; ++i) {
            if (!(handles_[i] == handle)) continue;
            for (std::size_t j = i + 1; j < tracked_; ++j)
                handles_[j - 1] = handles_[j];
            --tracked_;
            handles_[tracked_] = Handle{};
            return true;
        }
        return false;
    }

    /**
     * Deterministically drop one still-owned allocation and disarm its cleanup.
     */
    bool drop(const Handle& handle) noexcept {
        if (closed_) return false;
        for (std::size_t i = 0; i < tracked_; ++i) {
            if (!(handles_[i] == handle)) continue;
            if (!heap_->remove(handle)) return false;
            for (std::size_t j = i + 1; j < tracked_; ++j)
                handles_[j - 1] = handles_[j];
            --tracked_;
            handles_[tracked_] = Handle{};
            return true;
        }
        return false;
    }

    /**
     * Reverse-order cleanup. Returns false if a tracked handle was already
     * invalidated behind the scope's back; all remaining entries are still
     * attempted so one bad slot cannot suppress later cleanup.
     */
    bool close() noexcept {
        if (closed_) return cleanup_ok_;
        closed_ = true;
        while (tracked_ != 0) {
            const Handle handle = handles_[--tracked_];
            handles_[tracked_] = Handle{};
            if (!heap_->remove(handle)) cleanup_ok_ = false;
        }
        return cleanup_ok_;
    }

    std::size_t tracked() const noexcept { return tracked_; }
    bool closed() const noexcept { return closed_; }

private:
    Heap* heap_;
    std::array<Handle, MaxTracked> handles_{};
    std::size_t tracked_ = 0;
    bool closed_ = false;
    bool cleanup_ok_ = true;
};

} // namespace oreslang::runtime
