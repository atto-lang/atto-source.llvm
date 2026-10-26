#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "oreslang/runtime/DopFixedRecord.hpp"

namespace oreslang::dop {

template<class... Fields>
class InvertedColumns;

namespace detail {

template<class T>
inline constexpr bool inverted_scalar_field_v =
        std::is_same_v<std::remove_cv_t<T>, std::int32_t>
        || std::is_same_v<std::remove_cv_t<T>, std::int64_t>
        || std::is_same_v<std::remove_cv_t<T>, float>
        || std::is_same_v<std::remove_cv_t<T>, double>
        || std::is_same_v<std::remove_cv_t<T>, bool>;

/**
 * One process-wide owner sequence shared by every inverted-column field pack.
 * The counter is not host-mutable: only InvertedColumns can allocate IDs.
 */
class InvertedOwnerSequence final {
public:
    static std::uint64_t allocate() {
        std::uint64_t id = next_.load(std::memory_order_relaxed);
        while (true) {
            if (id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("inverted store identity exhausted");
            if (next_.compare_exchange_weak(
                    id, id + 1, std::memory_order_relaxed)) return id;
        }
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

} // namespace detail

// Native reference storage for the proposed Oreslang `inverted class` ABI.
// One typed contiguous vector per field, stable generation-checked handles,
// and field-granular loans. This is not yet source-language lowering.
template<class... Fields>
class InvertedColumns {
    static_assert(sizeof...(Fields) != 0, "an inverted class needs at least one field");
    static_assert(((detail::inverted_scalar_field_v<Fields>
                    || detail::is_fixed_record_v<Fields>) && ...),
                  "phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields");
    static_assert((std::is_trivially_copyable_v<Fields> && ...),
                  "phase-one columns require trivial field copies");
    static constexpr std::size_t field_count = sizeof...(Fields);
    // vector<bool> is bit-packed and has no data() span; represent bool
    // as canonical 0/1 bytes in the initial SoA ABI instead.
    template<class T> using Cell = std::conditional_t<std::is_same_v<T, bool>, std::uint8_t, T>;
    using Columns = std::tuple<std::vector<Cell<Fields>>...>;
    struct Slot {
        std::size_t row = 0;
        std::uint64_t generation = 1;
        bool live = false;
    };
    struct State {
        Columns columns;
        std::vector<std::size_t> row_slots;
        std::vector<Slot> slots;
        std::vector<std::size_t> free_slots;
        std::array<std::size_t, field_count> readers{};
        std::array<bool, field_count> writers{};
        std::mutex mutex;
        std::uint64_t owner;
        // Capability stays alive while any issued handle holds it; cannot be
        // reconstructed from predictable owner/slot/generation integers.
        std::shared_ptr<const unsigned char> issuer = std::make_shared<const unsigned char>(0);
        bool closed = false;
        explicit State(std::uint64_t id) : owner(id) {}
    };
    std::shared_ptr<State> state_;

    static void ensure_open(const State& s) {
        if (s.closed) throw std::logic_error("closed inverted column store");
    }
    static bool any_loans(const State& s) {
        for (std::size_t i = 0; i < field_count; ++i)
            if (s.readers[i] || s.writers[i]) return true;
        return false;
    }
    static std::size_t resolve(const State& s, const auto& h) {
        ensure_open(s);
        if (h.issuer_.get() != s.issuer.get() || h.owner_ != s.owner
                || h.slot_ >= s.slots.size())
            throw std::out_of_range("foreign or invalid inverted row handle");
        const auto& slot = s.slots[h.slot_];
        if (!slot.live || slot.generation != h.generation_)
            throw std::out_of_range("stale inverted row handle");
        return slot.row;
    }
    template<std::size_t... I>
    static void reserve_columns(State& s, std::size_t n, std::index_sequence<I...>) {
        (std::get<I>(s.columns).reserve(n), ...);
    }
    template<std::size_t... I>
    static void append_columns(State& s, const std::tuple<Fields...>& values,
                               std::index_sequence<I...>) {
        // All allocation was completed by reserve_columns. Each copy is a
        // no-throw trivial scalar/record copy, so appends commit together.
        (std::get<I>(s.columns).push_back(std::get<I>(values)), ...);
    }
    template<std::size_t... I>
    static std::tuple<Fields...> snapshot_columns(const State& s, std::size_t row,
                                                  std::index_sequence<I...>) {
        return std::tuple<Fields...>{std::get<I>(s.columns)[row]...};
    }
    template<std::size_t... I>
    static void update_columns(State& s, std::size_t row,
                               const std::tuple<Fields...>& values, std::index_sequence<I...>) {
        ((std::get<I>(s.columns)[row] = std::get<I>(values)), ...);
    }
    template<std::size_t... I>
    static void erase_columns(State& s, std::size_t row, std::size_t last,
                              std::index_sequence<I...>) {
        ((std::get<I>(s.columns)[row] = std::get<I>(s.columns)[last],
          // Scrub the vacated physical tail before releasing the logical row.
          std::get<I>(s.columns)[last] = Cell<std::tuple_element_t<I, std::tuple<Fields...>>>{},
          std::get<I>(s.columns).pop_back()), ...);
    }

public:
    class Handle {
        // Native host callers cannot mint row capabilities from public
        // numeric coordinates or mutate them to alias another live row.
        std::shared_ptr<const unsigned char> issuer_;
        std::uint64_t owner_;
        std::size_t slot_;
        std::uint64_t generation_;

        Handle(std::shared_ptr<const unsigned char> issuer, std::uint64_t owner,
               std::size_t slot, std::uint64_t generation)
            : issuer_(std::move(issuer)), owner_(owner), slot_(slot),
              generation_(generation) {}
        friend class InvertedColumns;
    public:
        std::uint64_t owner() const noexcept { return owner_; }
        std::size_t slot() const noexcept { return slot_; }
        std::uint64_t generation() const noexcept { return generation_; }
    };

    template<std::size_t I, bool Write>
    class Loan {
        static_assert(I < field_count, "invalid inverted column index");
        using Value = Cell<std::tuple_element_t<I, std::tuple<Fields...>>>;
        std::shared_ptr<State> state_;
        explicit Loan(std::shared_ptr<State> state) : state_(std::move(state)) {}
        friend class InvertedColumns;
    public:
        Loan(const Loan&) = delete;
        Loan& operator=(const Loan&) = delete;
        Loan(Loan&& other) noexcept : state_(std::move(other.state_)) {}
        Loan& operator=(Loan&&) = delete;
        ~Loan() { release(); }

        std::size_t size() const {
            if (!state_) throw std::logic_error("released inverted column loan");
            std::lock_guard<std::mutex> lock(state_->mutex);
            ensure_open(*state_);
            return std::get<I>(state_->columns).size();
        }

        Value get(std::size_t index) const {
            if (!state_) throw std::logic_error("released inverted column loan");
            std::lock_guard<std::mutex> lock(state_->mutex);
            ensure_open(*state_);
            const auto& column = std::get<I>(state_->columns);
            if (index >= column.size())
                throw std::out_of_range("inverted column loan index out of range");
            return column[index];
        }

        void set(std::size_t index, Value value) requires (Write) {
            if (!state_) throw std::logic_error("released inverted column loan");
            std::lock_guard<std::mutex> lock(state_->mutex);
            ensure_open(*state_);
            auto& column = std::get<I>(state_->columns);
            if (index >= column.size())
                throw std::out_of_range("inverted column loan index out of range");
            column[index] = value;
        }

        void release() noexcept {
            if (!state_) return;
            auto hold = std::move(state_);
            std::lock_guard<std::mutex> lock(hold->mutex);
            if constexpr (Write) hold->writers[I] = false;
            else --hold->readers[I];
        }
    };

    InvertedColumns()
        : state_(std::make_shared<State>(
                detail::InvertedOwnerSequence::allocate())) { }
    InvertedColumns(const InvertedColumns&) = delete;
    InvertedColumns& operator=(const InvertedColumns&) = delete;
    InvertedColumns(InvertedColumns&&) = delete;
    InvertedColumns& operator=(InvertedColumns&&) = delete;
    ~InvertedColumns() {
        // Outstanding loans retain backing state until their release. Their
        // checked accessors reject use after owner destruction; no raw pointer,
        // span, array reference, or mutable backing view escapes a loan.
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        return state_->row_slots.size();
    }
    /**
     * Checked dense bracket index to stable logical row handle.
     */
    Handle index(std::int64_t row_index) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        if (row_index < 0
                || row_index > std::numeric_limits<std::int32_t>::max()
                || static_cast<std::size_t>(row_index) >= state_->row_slots.size())
            throw std::out_of_range("inverted row index outside current length");
        const auto slot_index =
                state_->row_slots[static_cast<std::size_t>(row_index)];
        if (slot_index >= state_->slots.size())
            throw std::logic_error("corrupt inverted dense-row mapping");
        const auto& slot = state_->slots[slot_index];
        if (!slot.live || slot.row != static_cast<std::size_t>(row_index))
            throw std::logic_error("corrupt inverted dense-row mapping");
        return Handle{state_->issuer, state_->owner, slot_index, slot.generation};
    }
    // Explicit growth avoids reallocations inside future append batches.
    // Reserving any field is structural and excludes every outstanding loan.
    void reserve(std::size_t capacity) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        State& s = *state_;
        ensure_open(s);
        if (any_loans(s)) throw std::logic_error("cannot reserve borrowed inverted columns");
        s.row_slots.reserve(capacity);
        s.slots.reserve(capacity);
        reserve_columns(s, capacity, std::index_sequence_for<Fields...>{});
    }
    Handle push(Fields... fields) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        State& s = *state_;
        ensure_open(s);
        if (any_loans(s)) throw std::logic_error("cannot grow borrowed inverted columns");
        const auto row = s.row_slots.size();
        if (row == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("inverted row count exhausted");
        // Allocate everything before changing logical size. If allocation
        // throws, no row/slot/column sizes change.
        s.row_slots.reserve(row + 1);
        if (s.free_slots.empty()) s.slots.reserve(s.slots.size() + 1);
        reserve_columns(s, row + 1, std::index_sequence_for<Fields...>{});
        const bool reuse = !s.free_slots.empty();
        const auto slot = reuse ? s.free_slots.back() : s.slots.size();
        append_columns(s, std::tuple<Fields...>(fields...), std::index_sequence_for<Fields...>{});
        if (reuse) {
            s.free_slots.pop_back();
            s.slots[slot].row = row;
            s.slots[slot].live = true;
        } else {
            s.slots.push_back(Slot{row, 1, true});
        }
        s.row_slots.push_back(slot);
        return Handle{s.issuer, s.owner, slot, s.slots[slot].generation};
    }
    void erase_swap(Handle h) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        State& s = *state_;
        const auto row = resolve(s, h);
        if (any_loans(s)) throw std::logic_error("cannot erase borrowed inverted columns");
        const auto last = s.row_slots.size() - 1;
        const bool recycle = s.slots[h.slot_].generation != std::numeric_limits<std::uint64_t>::max();
        if (recycle) s.free_slots.reserve(s.free_slots.size() + 1);
        erase_columns(s, row, last, std::index_sequence_for<Fields...>{});
        if (row != last) {
            const auto displaced = s.row_slots[last];
            s.row_slots[row] = displaced;
            s.slots[displaced].row = row;
        }
        s.row_slots.pop_back();
        auto& slot = s.slots[h.slot_];
        slot.live = false;
        if (recycle) {
            ++slot.generation;
            s.free_slots.push_back(h.slot_);
        }
    }
    // A complete row snapshot observes a single checked storage state.
    // An outstanding mutable field loan prevents snapshotting the row.
    std::tuple<Fields...> get_row(Handle h) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto row = resolve(*state_, h);
        for (bool writer : state_->writers)
            if (writer) throw std::logic_error("row has a mutably borrowed column");
        return snapshot_columns(*state_, row, std::index_sequence_for<Fields...>{});
    }
    // Single-lock update of all columns: no reader observes partially updated
    // rows. A complete update requires exclusivity over all columns.
    void set_row(Handle h, Fields... fields) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto row = resolve(*state_, h);
        if (any_loans(*state_)) throw std::logic_error("row update conflicts with active column loan");
        update_columns(*state_, row, std::tuple<Fields...>(fields...),
                       std::index_sequence_for<Fields...>{});
    }
    template<std::size_t I>
    auto get(Handle h) const -> std::tuple_element_t<I, std::tuple<Fields...>> {
        static_assert(I < field_count);
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto row = resolve(*state_, h);
        if (state_->writers[I]) throw std::logic_error("column has a mutable loan");
        return std::get<I>(state_->columns)[row];
    }
    template<std::size_t I>
    void set(Handle h, std::tuple_element_t<I, std::tuple<Fields...>> value) {
        static_assert(I < field_count);
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto row = resolve(*state_, h);
        if (state_->writers[I] || state_->readers[I])
            throw std::logic_error("column has an outstanding loan");
        std::get<I>(state_->columns)[row] = value;
    }
    template<std::size_t I>
    Loan<I, false> borrow_read() {
        static_assert(I < field_count);
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        if (state_->writers[I]) throw std::logic_error("column already mutably borrowed");
        if (state_->readers[I] == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("inverted column reader count exhausted");
        ++state_->readers[I];
        return Loan<I, false>(state_);
    }
    template<std::size_t I>
    Loan<I, true> borrow_write() {
        static_assert(I < field_count);
        using Field = std::tuple_element_t<I, std::tuple<Fields...>>;
        static_assert(!std::is_same_v<Field, bool> && !detail::is_fixed_record_v<Field>,
                      "bool/record byte lanes require canonical writes via set; mutable spans are disallowed");
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        if (state_->writers[I] || state_->readers[I])
            throw std::logic_error("column already borrowed");
        state_->writers[I] = true;
        return Loan<I, true>(state_);
    }
};

} // namespace oreslang::dop
