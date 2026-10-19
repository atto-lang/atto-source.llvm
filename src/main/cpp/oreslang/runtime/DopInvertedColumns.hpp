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

namespace oreslang::dop {

// Shared across all field type packs. Never wrap and reuse a live or stale
// handle's owner identity, even after a store is destroyed.
inline std::atomic<std::uint64_t> next_inverted_store_owner{1};

// Native reference storage for the proposed Oreslang `inverted class` ABI.
// One typed contiguous vector per field, stable generation-checked handles,
// and field-granular loans. This is not yet source-language lowering.
template<class... Fields>
class InvertedColumns {
    static_assert(sizeof...(Fields) != 0, "an inverted class needs at least one field");
    static_assert(((std::is_arithmetic_v<Fields> || std::is_enum_v<Fields>) && ...),
                  "phase-one columns only support pointer-free arithmetic and enum fields");
    static_assert((std::is_trivially_copyable_v<Fields> && ...),
                  "phase-one columns require trivial field copies");
    static_assert(((sizeof(Fields) == 1 || sizeof(Fields) == 2 ||
                    sizeof(Fields) == 4 || sizeof(Fields) == 8) && ...),
                  "phase-one column cells must have a fixed 1/2/4/8 byte width");
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
        if (h.owner != s.owner || h.slot >= s.slots.size())
            throw std::out_of_range("foreign or invalid inverted row handle");
        const auto& slot = s.slots[h.slot];
        if (!slot.live || slot.generation != h.generation)
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
    static void erase_columns(State& s, std::size_t row, std::size_t last,
                              std::index_sequence<I...>) {
        ((std::get<I>(s.columns)[row] = std::get<I>(s.columns)[last],
          std::get<I>(s.columns).pop_back()), ...);
    }

public:
    struct Handle {
        std::uint64_t owner;
        std::size_t slot;
        std::uint64_t generation;
    };

    template<std::size_t I, bool Write>
    class Loan {
        static_assert(I < field_count, "invalid inverted column index");
        using Value = Cell<std::tuple_element_t<I, std::tuple<Fields...>>>;
        using SpanValue = std::conditional_t<Write, Value, const Value>;
        std::shared_ptr<State> state_;
        explicit Loan(std::shared_ptr<State> state) : state_(std::move(state)) {}
        friend class InvertedColumns;
    public:
        Loan(const Loan&) = delete;
        Loan& operator=(const Loan&) = delete;
        Loan(Loan&& other) noexcept : state_(std::move(other.state_)) {}
        Loan& operator=(Loan&&) = delete;
        ~Loan() { release(); }
        std::span<SpanValue> span() const {
            if (!state_) throw std::logic_error("released inverted column loan");
            // A loan retains backing memory but cannot issue a *new* span after
            // its owning store has been destroyed. Already-issued raw C++ spans
            // cannot be revoked: the source borrow checker must prevent escape.
            std::lock_guard<std::mutex> lock(state_->mutex);
            ensure_open(*state_);
            auto& column = std::get<I>(state_->columns);
            return {column.data(), column.size()};
        }
        void release() noexcept {
            if (!state_) return;
            auto hold = std::move(state_);
            std::lock_guard<std::mutex> lock(hold->mutex);
            if constexpr (Write) hold->writers[I] = false;
            else --hold->readers[I];
        }
    };

    InvertedColumns() {
        std::uint64_t id = next_inverted_store_owner.load(std::memory_order_relaxed);
        while (true) {
            if (id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("inverted store identity exhausted");
            if (next_inverted_store_owner.compare_exchange_weak(
                    id, id + 1, std::memory_order_relaxed)) break;
        }
        state_ = std::make_shared<State>(id);
    }
    InvertedColumns(const InvertedColumns&) = delete;
    InvertedColumns& operator=(const InvertedColumns&) = delete;
    InvertedColumns(InvertedColumns&&) = delete;
    InvertedColumns& operator=(InvertedColumns&&) = delete;
    ~InvertedColumns() {
        // Outstanding loans retain backing state until their release, so
        // destruction cannot free memory under a borrower.
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        return state_->row_slots.size();
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
        return Handle{s.owner, slot, s.slots[slot].generation};
    }
    void erase_swap(Handle h) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        State& s = *state_;
        const auto row = resolve(s, h);
        if (any_loans(s)) throw std::logic_error("cannot erase borrowed inverted columns");
        const auto last = s.row_slots.size() - 1;
        const bool recycle = s.slots[h.slot].generation != std::numeric_limits<std::uint64_t>::max();
        if (recycle) s.free_slots.reserve(s.free_slots.size() + 1);
        erase_columns(s, row, last, std::index_sequence_for<Fields...>{});
        if (row != last) {
            const auto displaced = s.row_slots[last];
            s.row_slots[row] = displaced;
            s.slots[displaced].row = row;
        }
        s.row_slots.pop_back();
        auto& slot = s.slots[h.slot];
        slot.live = false;
        if (recycle) {
            ++slot.generation;
            s.free_slots.push_back(h.slot);
        }
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
        static_assert(!std::is_same_v<std::tuple_element_t<I, std::tuple<Fields...>>, bool>,
                      "bool byte lanes require canonical writes via set; mutable spans are disallowed");
        std::lock_guard<std::mutex> lock(state_->mutex);
        ensure_open(*state_);
        if (state_->writers[I] || state_->readers[I])
            throw std::logic_error("column already borrowed");
        state_->writers[I] = true;
        return Loan<I, true>(state_);
    }
};

} // namespace oreslang::dop
