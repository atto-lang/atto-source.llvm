#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// INTERNAL host primitive. The caller must hold the physical mailbox lock;
// this class intentionally has no separate lock, scheduler or guest callback.
// Each enqueue consumes one already-allocated slot and transfers a pre-owned
// payload handle. No allocator or user T move is used on a successful path.
// It does not implement actor ACK accounting, authenticated CONTROL, or
// multi-lane Erlang selective receive. All three belong to outer layers.
template<class T>
class PreallocatedSelectiveSlots final {
public:
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    // The native actor-turn admission gate already caps capacity at 65,536.
    // Reject unbounded/invalid requests BEFORE allocating the slot vector.
    static constexpr std::size_t max_capacity = 65536;
    struct Frame {
        std::uint64_t sequence;
        std::uint32_t tag;
        std::size_t bytes;
        std::unique_ptr<T> payload;
    };
    enum class Status { Matched, Continue, NoMatch };
    class Cursor final {
    public:
        Cursor() = default;
        std::uint64_t through_sequence() const noexcept { return through_; }
    private:
        friend class PreallocatedSelectiveSlots<T>;
        Cursor(std::uint64_t owner, std::size_t next, std::uint64_t generation,
               std::uint64_t through, std::uint32_t tag) noexcept
            : owner_(owner), generation_(generation), through_(through),
              next_(next), tag_(tag), valid_(true) {}
        std::uint64_t owner_ = 0, generation_ = 0, through_ = 0;
        std::size_t next_ = none;
        std::uint32_t tag_ = 0;
        bool valid_ = false;
    };
    struct Result {
        Status status;
        std::optional<Frame> frame;
        std::size_t inspected;
        Cursor cursor;
    };

    explicit PreallocatedSelectiveSlots(std::size_t capacity)
        : slots_(checked_capacity(capacity)), owner_(new_owner()) {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            slots_[i].free_next = i + 1 == slots_.size() ? none : i + 1;
        free_ = 0;
    }
    PreallocatedSelectiveSlots(const PreallocatedSelectiveSlots&) = delete;
    PreallocatedSelectiveSlots& operator=(const PreallocatedSelectiveSlots&) = delete;
    PreallocatedSelectiveSlots(PreallocatedSelectiveSlots&&) = delete;
    PreallocatedSelectiveSlots& operator=(PreallocatedSelectiveSlots&&) = delete;

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    bool push(std::unique_ptr<T>& payload, std::size_t bytes,
              std::uint32_t tag, std::uint64_t sequence) {
        if (!payload || bytes == 0 || sequence == 0 || sequence <= last_sequence_)
            throw std::invalid_argument("invalid selective frame or sequence");
        if (free_ == none) return false;
        const auto i = free_;
        Slot& slot = slots_[i];
        if (slot.frame || slot.generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("selective slot identity exhausted");
        free_ = slot.free_next;
        slot.generation++;
        slot.prev = tail_;
        slot.next = none;
        slot.frame.emplace(Frame{sequence, tag, bytes, std::move(payload)});
        if (tail_ != none) slots_[tail_].next = i;
        else head_ = i;
        tail_ = i;
        last_sequence_ = sequence;
        ++size_;
        return true;
    }

    std::optional<Frame> pop_front() {
        if (head_ == none) return std::nullopt;
        return detach(head_);
    }

    Result select(std::uint32_t tag, std::size_t budget,
                  const Cursor& cursor = {}) {
        if (tag == 0 || budget == 0)
            throw std::invalid_argument("typed tag and positive selective scan budget required");
        std::size_t i = head_;
        std::uint64_t through = 0;
        if (cursor.valid_) {
            if (cursor.owner_ != owner_ || cursor.tag_ != tag ||
                cursor.next_ >= slots_.size() ||
                !slots_[cursor.next_].frame ||
                slots_[cursor.next_].generation != cursor.generation_ ||
                slots_[cursor.next_].frame->sequence <= cursor.through_)
                throw std::invalid_argument("stale or forged selective cursor");
            i = cursor.next_;
            through = cursor.through_;
        }
        std::size_t checked = 0;
        while (i != none) {
            if (checked == budget)
                return {Status::Continue, std::nullopt, checked,
                        Cursor(owner_, i, slots_[i].generation, through, tag)};
            ++checked;
            through = slots_[i].frame->sequence;
            if (slots_[i].frame->tag == tag)
                return {Status::Matched, detach(i), checked, {}};
            i = slots_[i].next;
        }
        // NoMatch never returns an enduring cursor; later appended messages
        // must be visible on a fresh scan.
        return {Status::NoMatch, std::nullopt, checked, {}};
    }

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0 || capacity > max_capacity)
            throw std::invalid_argument("actor mailbox capacity outside bounded range");
        return capacity;
    }
    struct Slot {
        std::optional<Frame> frame;
        std::size_t prev = none, next = none, free_next = none;
        std::uint64_t generation = 0;
    };
    static std::uint64_t new_owner() {
        static std::atomic<std::uint64_t> owners{1};
        auto id = owners.load(std::memory_order_relaxed);
        for (;;) {
            // Do NOT fetch_add first and test later: that wraps the counter
            // and eventually permits old mailbox owner IDs to be reused.
            if (id == 0 || id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("mailbox incarnation sequence exhausted");
            if (owners.compare_exchange_weak(id, id + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return id;
        }
    }
    std::optional<Frame> detach(std::size_t i) {
        Slot& slot = slots_[i];
        if (!slot.frame) throw std::logic_error("selective free slot dequeued");
        std::optional<Frame> moved(std::move(*slot.frame));
        if (slot.prev != none) slots_[slot.prev].next = slot.next;
        else head_ = slot.next;
        if (slot.next != none) slots_[slot.next].prev = slot.prev;
        else tail_ = slot.prev;
        slot.frame.reset(); // payload was moved out: never run T destructor here
        slot.prev = slot.next = none;
        slot.free_next = free_;
        free_ = i;
        --size_;
        return moved;
    }
    std::vector<Slot> slots_;
    const std::uint64_t owner_;
    std::size_t free_ = none, head_ = none, tail_ = none, size_ = 0;
    std::uint64_t last_sequence_ = 0;
};

} // namespace oreslang::runtime
