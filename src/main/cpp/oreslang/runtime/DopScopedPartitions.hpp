#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Reference CPU storage/borrow admission for independently scheduled ECS/DOP
// Tasks. This is NOT yet an Oreslang compiler borrow proof or TaskScope ABI.
// A source compiler must keep returned spans from escaping the owning lease.
template<class T>
class ScopedColumn {
    static_assert((std::is_arithmetic_v<T> || std::is_enum_v<T>) &&
                  !std::is_same_v<T, bool> && std::is_trivially_copyable_v<T> &&
                  (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8),
                  "scoped column cells must be fixed-width pointer-free POD (not bool)");
    struct Range {
        std::size_t first;
        std::size_t end;  // exclusive
        std::uint64_t ticket;
        bool write;
    };
    struct State {
        std::mutex mutex;
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
    static void check_index(const State& s, std::size_t i) {
        if (i >= s.cells.size()) throw std::out_of_range("column index out of range");
    }
    static void check_range(const State& s, std::size_t first, std::size_t count) {
        // Subtraction form prevents overflow on first + count.
        if (first > s.cells.size() || count > s.cells.size() - first)
            throw std::out_of_range("column partition out of range");
    }
    template<bool Write>
    static bool conflicting(const Range& r, std::size_t first, std::size_t end) {
        return overlaps(r.first, r.end, first, end) && (Write || r.write);
    }

public:
    template<bool Write>
    class Lease {
        using Element = std::conditional_t<Write, T, const T>;
        std::shared_ptr<State> state_;
        std::size_t first_ = 0;
        std::size_t count_ = 0;
        std::uint64_t ticket_ = 0;
        Lease(std::shared_ptr<State> state, std::size_t first,
              std::size_t count, std::uint64_t ticket) noexcept
            : state_(std::move(state)), first_(first), count_(count), ticket_(ticket) {}
        friend class ScopedColumn;
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), first_(other.first_),
              count_(other.count_), ticket_(std::exchange(other.ticket_, 0)) {}
        Lease& operator=(Lease&&) = delete;
        ~Lease() { release(); }
        std::size_t size() const noexcept { return count_; }
        std::span<Element> span() const {
            if (!state_) throw std::logic_error("released partition lease");
            std::lock_guard lock(state_->mutex);
            require_open(*state_);
            const auto found = std::find_if(state_->active.begin(), state_->active.end(),
                [&](const Range& r) { return r.ticket == ticket_; });
            if (found == state_->active.end())
                throw std::logic_error("inactive partition lease");
            if (count_ == 0) return {};
            return {state_->cells.data() + first_, count_};
        }
        void release() noexcept {
            if (!state_) return;
            auto owner = std::move(state_);
            std::lock_guard lock(owner->mutex);
            const auto found = std::find_if(owner->active.begin(), owner->active.end(),
                [&](const Range& r) { return r.ticket == ticket_; });
            if (found != owner->active.end()) owner->active.erase(found);
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
        // Leases pin the storage, but no new span is issued after retirement.
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
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
        check_index(*state_, index);
        for (const auto& r : state_->active)
            if (r.write && overlaps(r.first, r.end, index, index + 1))
                throw std::logic_error("reading a mutably borrowed cell");
        return state_->cells[index];
    }
    void set(std::size_t index, T value) {
        std::lock_guard lock(state_->mutex);
        require_open(*state_);
        check_index(*state_, index);
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
        const auto end = first + count;  // proved in bounds above
        for (const auto& r : s.active)
            if (conflicting<Write>(r, first, end))
                throw std::logic_error("overlapping partition borrow");
        // Admission is atomic: a failed vector allocation creates no lease.
        const auto ticket = s.next_ticket;
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
