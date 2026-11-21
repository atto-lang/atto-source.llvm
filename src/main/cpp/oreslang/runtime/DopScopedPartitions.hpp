#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Host reference for actor-independent DOP/ECS partition admission.  Not an
// Oreslang source-level borrow checker, TaskScope, or an FFI surface.
// Safe methods never give callers pointers/references to column storage.
template<class T>
class ScopedColumn {
    static_assert((std::is_arithmetic_v<T> || std::is_enum_v<T>) &&
                  !std::is_same_v<T, bool> && std::is_trivially_copyable_v<T> &&
                  (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8),
                  "only fixed-width pointer-free arithmetic/enum cells (not bool)");
    struct Range {
        std::size_t first;
        std::size_t end;                 // exclusive
        std::uint64_t ticket;           // monotonically increasing, never reused
        bool write;
        bool executing = false;         // one bulk callback per lease
        bool pending_release = false;   // callback may release its own lease
    };
    struct State {
        std::mutex mutex;               // admission/metadata only; never held in a hot loop
        std::vector<T> cells;
        std::vector<Range> active;
        std::uint64_t next_ticket = 1;
        bool closed = false;
        explicit State(std::size_t size) : cells(size) {}
    };
    std::shared_ptr<State> state_;

    static void require_open(const State& s) {
        if (s.closed) throw std::logic_error("closed scoped column");
    }
    static bool overlaps(std::size_t a, std::size_t b,
                         std::size_t x, std::size_t y) noexcept {
        return a < y && x < b;
    }
    static void check_range(const State& s, std::size_t first, std::size_t count) {
        if (first > s.cells.size() || count > s.cells.size() - first)
            throw std::out_of_range("column partition out of range");
    }
    template<bool Write>
    static bool conflicting(const Range& r, std::size_t first, std::size_t end) {
        return overlaps(r.first, r.end, first, end) && (Write || r.write);
    }
    static auto find_lease(State& s, std::uint64_t ticket) {
        return std::find_if(s.active.begin(), s.active.end(),
                            [ticket](const Range& r) { return r.ticket == ticket; });
    }

public:
    template<bool Write>
    class Lease {
        std::shared_ptr<State> state_;
        std::size_t first_ = 0;
        std::size_t count_ = 0;
        std::uint64_t ticket_ = 0;
        Lease(std::shared_ptr<State> state, std::size_t first,
              std::size_t count, std::uint64_t ticket) noexcept
            : state_(std::move(state)), first_(first), count_(count), ticket_(ticket) {}
        friend class ScopedColumn;

        // Keeps state pinned during a callback and guarantees that a reentrant
        // release cannot unlock/reallocate the buffer while the callback runs.
        class Operation {
            std::shared_ptr<State> state_;
            std::uint64_t ticket_;
        public:
            Operation(const Lease& lease) : state_(lease.state_), ticket_(lease.ticket_) {
                if (!state_) throw std::logic_error("released partition lease");
                std::lock_guard lock(state_->mutex);
                require_open(*state_);
                auto it = find_lease(*state_, ticket_);
                if (it == state_->active.end() || it->pending_release)
                    throw std::logic_error("inactive partition lease");
                if (it->executing)
                    throw std::logic_error("partition lease already executing");
                it->executing = true;
            }
            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            ~Operation() {
                std::lock_guard lock(state_->mutex);
                auto it = find_lease(*state_, ticket_);
                if (it == state_->active.end()) return;
                if (it->pending_release) state_->active.erase(it);
                else it->executing = false;
            }
        };

    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), first_(other.first_),
              count_(other.count_), ticket_(std::exchange(other.ticket_, 0)) {}
        Lease& operator=(Lease&&) = delete;
        ~Lease() { release(); }
        std::size_t size() const {
            if (!state_) throw std::logic_error("released partition lease");
            std::lock_guard lock(state_->mutex);
            require_open(*state_);
            auto it = find_lease(*state_, ticket_);
            if (it == state_->active.end() || it->pending_release)
                throw std::logic_error("inactive partition lease");
            return count_;
        }

        // Callback gets a copy of each cell, never a raw alias to the backing
        // buffer. Template inlining preserves a contiguous hot-loop substrate.
        template<class Fn>
        void for_each(Fn&& fn) const {
            static_assert(std::is_invocable_v<Fn&, std::size_t, T>,
                          "read callback accepts (index, cell-value)");
            Operation pin(*this);
            const auto& cells = state_->cells;
            for (std::size_t i = 0; i < count_; ++i)
                std::invoke(fn, i, static_cast<T>(cells[first_ + i]));
        }

        // Explicit value-in/value-out write loop: cannot return a reference
        // into this column. No mutex/atomic operation occurs per cell.
        template<class Fn>
        void transform(Fn&& fn) requires (Write) {
            static_assert(std::is_invocable_r_v<T, Fn&, std::size_t, T>,
                          "transform callback returns cell-value from (index, cell-value)");
            Operation pin(*this);
            auto& cells = state_->cells;
            for (std::size_t i = 0; i < count_; ++i) {
                const std::size_t index = first_ + i;
                const T input = cells[index];
                const T output = static_cast<T>(std::invoke(fn, i, input));
                cells[index] = output;
            }
        }

        // A release reentered from a running callback is deferred to callback
        // exit. Thus another owner cannot resize between callback iterations.
        void release() noexcept {
            if (!state_) return;
            auto owner = std::move(state_);
            std::lock_guard lock(owner->mutex);
            auto it = find_lease(*owner, ticket_);
            if (it != owner->active.end()) {
                if (it->executing) it->pending_release = true;
                else owner->active.erase(it);
            }
            ticket_ = 0;
        }
    };
    using ReadLease = Lease<false>;
    using WriteLease = Lease<true>;

    explicit ScopedColumn(std::size_t size) : state_(std::make_shared<State>(size)) {}
    ScopedColumn(const ScopedColumn&) = delete;
    ScopedColumn& operator=(const ScopedColumn&) = delete;
    ScopedColumn(ScopedColumn&&) = delete;
    ScopedColumn& operator=(ScopedColumn&&) = delete;
    ~ScopedColumn() {
        std::lock_guard lock(state_->mutex);
        state_->closed = true; // new accesses fail; active callbacks finish pinned
    }
    std::size_t size() const {
        std::lock_guard lock(state_->mutex);
        require_open(*state_);
        return state_->cells.size();
    }
    void resize(std::size_t count) {
        std::lock_guard lock(state_->mutex);
        require_open(*state_);
        if (!state_->active.empty())
            throw std::logic_error("structural mutation while partitions are borrowed");
        state_->cells.resize(count);
    }
    T get(std::size_t index) const {
        std::lock_guard lock(state_->mutex);
        require_open(*state_);
        check_range(*state_, index, 1);
        for (const auto& r : state_->active)
            if (r.write && overlaps(r.first, r.end, index, index + 1))
                throw std::logic_error("reading a mutably borrowed cell");
        return state_->cells[index];
    }
    void set(std::size_t index, T value) {
        std::lock_guard lock(state_->mutex);
        require_open(*state_);
        check_range(*state_, index, 1);
        for (const auto& r : state_->active)
            if (overlaps(r.first, r.end, index, index + 1))
                throw std::logic_error("writing a borrowed cell");
        state_->cells[index] = value;
    }
    template<bool Write>
    Lease<Write> borrow(std::size_t first, std::size_t count) {
        std::lock_guard lock(state_->mutex);
        State& s = *state_;
        require_open(s);
        check_range(s, first, count);
        if (s.next_ticket == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("partition ticket exhausted");
        const auto end = first + count; // proven not to overflow
        for (const auto& r : s.active)
            if (conflicting<Write>(r, first, end))
                throw std::logic_error("overlapping partition borrow");
        const auto ticket = s.next_ticket;
        // Failed allocation leaves the lease set and ticket unchanged.
        s.active.push_back(Range{first, end, ticket, Write});
        ++s.next_ticket;
        return Lease<Write>(state_, first, count, ticket);
    }
    ReadLease read(std::size_t first, std::size_t count) {
        return borrow<false>(first, count);
    }
    WriteLease write(std::size_t first, std::size_t count) {
        return borrow<true>(first, count);
    }
};

} // namespace oreslang::dop
