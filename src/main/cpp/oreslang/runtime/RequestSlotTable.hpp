#pragma once
// Host-only transport storage. No Oreslang guest-source or actor-mail lowering is implied.
// Fixed-capacity buffers are allocated once at table construction, never per request.
// Raw views are valid only while the corresponding read lease is live. No await/escape.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {
template <std::size_t Slots, std::size_t MaxBytes>
class RequestSlotTable final {
    static_assert(Slots > 0 && Slots <= UINT32_MAX, "invalid slot count");
    static_assert(MaxBytes > 0, "invalid buffer size");
public:
    // A value, NOT a pointer or authentication token. Verify owner against the
    // trusted runtime's current actor identity on every operation.
    struct Handle { std::uint64_t value = 0; };
    static_assert(sizeof(Handle) == 8, "request handles must remain compact");
private:
    enum class Phase : std::uint8_t { free, writing, aborting, ready, closing, retired };
    struct Slot {
        std::array<char, MaxBytes> bytes{};
        std::uint32_t generation = 0;
        std::uint64_t owner = 0;
        std::size_t size = 0;
        std::size_t readers = 0;
        Phase phase = Phase::free;
    };
    std::unique_ptr<Slot[]> slots_;
    std::array<std::uint32_t, Slots> available_{};
    std::size_t head_ = 0, tail_ = 0, free_count_ = Slots;
    mutable std::mutex mutex_;
    static Handle make(std::size_t index, std::uint32_t generation) noexcept {
        return {(static_cast<std::uint64_t>(generation) << 32) |
                static_cast<std::uint32_t>(index + 1)};
    }
    static std::size_t index(Handle h) noexcept {
        return static_cast<std::uint32_t>(h.value) - 1u;
    }
    Slot* lookup(Handle h) noexcept {
        const auto low = static_cast<std::uint32_t>(h.value);
        if (low == 0 || low > Slots) return nullptr;
        auto& slot = slots_[index(h)];
        if (slot.generation != (h.value >> 32)) return nullptr;
        return &slot;
    }
    void recycle(std::size_t i) noexcept { // caller holds mutex_
        auto& slot = slots_[i];
        // Queue corruption is fatal even under NDEBUG: silent ring overflow
        // could hand out still-owned request buffers to unrelated actors.
        if (slot.readers != 0 || free_count_ >= Slots ||
            (slot.phase != Phase::closing && slot.phase != Phase::writing &&
             slot.phase != Phase::aborting)) std::terminate();
        slot.owner = 0;
        slot.size = 0;
        if (slot.generation == UINT32_MAX) {
            slot.phase = Phase::retired; // never allow generation wrap / stale resurrection
            return;
        }
        slot.phase = Phase::free;
        available_[tail_] = static_cast<std::uint32_t>(i);
        tail_ = (tail_ + 1) % Slots;
        ++free_count_;
    }
    void abandon(std::size_t i, std::uint32_t generation) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& slot = slots_[i];
        if (slot.generation == generation &&
            (slot.phase == Phase::writing || slot.phase == Phase::aborting))
            recycle(i);
    }
    void drop_read(std::size_t i) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& slot = slots_[i];
        // Never let a bookkeeping underflow resurrect an already-freed slot.
        if (slot.readers == 0 ||
            (slot.phase != Phase::ready && slot.phase != Phase::closing))
            std::terminate();
        --slot.readers;
        if (slot.readers == 0 && slot.phase == Phase::closing) recycle(i);
    }
public:
    class Ingress final {
        friend class RequestSlotTable;
        RequestSlotTable* table_ = nullptr;
        std::size_t index_ = 0;
        std::uint32_t generation_ = 0;
        Ingress(RequestSlotTable* table, std::size_t i, std::uint32_t gen) noexcept
            : table_(table), index_(i), generation_(gen) {}
    public:
        Ingress() = default;
        Ingress(const Ingress&) = delete;
        Ingress& operator=(const Ingress&) = delete;
        Ingress(Ingress&& other) noexcept
            : table_(std::exchange(other.table_, nullptr)),
              index_(other.index_), generation_(other.generation_) {}
        Ingress& operator=(Ingress&& other) noexcept {
            if (this != &other) {
                reset(); table_ = std::exchange(other.table_, nullptr);
                index_ = other.index_; generation_ = other.generation_;
            }
            return *this;
        }
        ~Ingress() { reset(); }
        void reset() noexcept {
            if (auto* table = std::exchange(table_, nullptr))
                table->abandon(index_, generation_);
        }
        char* data() noexcept { return table_ ? table_->slots_[index_].bytes.data() : nullptr; }
        std::size_t capacity() const noexcept { return table_ ? MaxBytes : 0; }
        // Publishing freezes the ingress bytes; no writer or alias may mutate them.
        Handle publish(std::size_t bytes) noexcept {
            if (!table_ || bytes > MaxBytes) return {};
            std::lock_guard<std::mutex> guard(table_->mutex_);
            auto& slot = table_->slots_[index_];
            if (slot.phase != Phase::writing || slot.generation != generation_) return {};
            slot.size = bytes;
            slot.phase = Phase::ready;
            table_ = nullptr;
            return make(index_, generation_);
        }
    };
    class ReadLease final {
        friend class RequestSlotTable;
        RequestSlotTable* table_ = nullptr;
        std::size_t index_ = 0;
        ReadLease(RequestSlotTable* table, std::size_t i) noexcept
            : table_(table), index_(i) {}
    public:
        ReadLease() = default;
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept
            : table_(std::exchange(other.table_, nullptr)), index_(other.index_) {}
        ReadLease& operator=(ReadLease&& other) noexcept {
            if (this != &other) {
                reset(); table_ = std::exchange(other.table_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        ~ReadLease() { reset(); }
        void reset() noexcept {
            if (auto* table = std::exchange(table_, nullptr)) table->drop_read(index_);
        }
        // Borrowed view, never ownable guest String; guest compiler must reject
        // storing, returning, sending or crossing a suspend with this view.
        std::string_view view() const noexcept {
            if (!table_) return {};
            const auto& slot = table_->slots_[index_];
            return {slot.bytes.data(), slot.size};
        }
        explicit operator bool() const noexcept { return table_ != nullptr; }
    };
    RequestSlotTable() : slots_(std::make_unique<Slot[]>(Slots)) {
        for (std::size_t i = 0; i < Slots; ++i)
            available_[i] = static_cast<std::uint32_t>(i);
    }
    RequestSlotTable(const RequestSlotTable&) = delete;
    RequestSlotTable& operator=(const RequestSlotTable&) = delete;
    ~RequestSlotTable() {
        // Transport shutdown must quiesce every ingress/borrow before teardown.
        for (std::size_t i = 0; i < Slots; ++i)
            if (slots_[i].readers != 0 ||
                (slots_[i].phase != Phase::free && slots_[i].phase != Phase::retired))
                std::terminate(); // never free transport storage under an active loan
    }
    std::optional<Ingress> begin(std::uint64_t owner) {
        if (owner == 0) return std::nullopt;
        std::lock_guard<std::mutex> guard(mutex_);
        if (free_count_ == 0) return std::nullopt;
        const auto i = available_[head_];
        head_ = (head_ + 1) % Slots;
        --free_count_;
        auto& slot = slots_[i];
        // Production builds must reject a broken free-list, not rely on
        // assert() being enabled. Generation exhaustion never wraps.
        if (slot.phase != Phase::free || slot.generation == UINT32_MAX ||
            slot.readers != 0) std::terminate();
        ++slot.generation;
        slot.owner = owner;
        slot.phase = Phase::writing;
        return Ingress(this, i, slot.generation);
    }
    std::optional<ReadLease> borrow(Handle h, std::uint64_t owner) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* slot = lookup(h);
        if (!slot || owner == 0 || slot->owner != owner || slot->phase != Phase::ready)
            return std::nullopt;
        ++slot->readers;
        return ReadLease(this, index(h));
    }
    // Trusted-native lexical loan helper, intended as a lowering target for
    // `using request.borrow() as view { ... }`. It performs no guest execution
    // and does not itself prove that a C++ callback cannot capture/escape a
    // string_view. The source compiler must make that proof independently.
    // Requiring void return avoids inadvertently returning a dangling view.
    template <typename Visitor>
    bool with_borrow(Handle h, std::uint64_t owner, Visitor&& visitor) {
        static_assert(std::is_same_v<
                std::invoke_result_t<Visitor&, std::string_view>, void>,
                "borrow scope callback cannot return a value or borrowed view");
        auto loan = borrow(h, owner);
        if (!loan) return false;
        std::invoke(std::forward<Visitor>(visitor), loan->view());
        // RAII returns the lease even when visitor throws. Never hold the
        // table mutex while invoking arbitrary trusted native code.
        return true;
    }
    // Handoff invalidates the old handle and all old-owner authority.
    // Cannot transfer during an active view lease.
    std::optional<Handle> transfer(Handle h, std::uint64_t from, std::uint64_t to) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* slot = lookup(h);
        if (!slot || from == 0 || to == 0 || from == to || slot->owner != from ||
            slot->phase != Phase::ready || slot->readers != 0 ||
            slot->generation == UINT32_MAX) return std::nullopt;
        ++slot->generation;
        slot->owner = to;
        return make(index(h), slot->generation);
    }
    // Idempotent reclamation is internal; an externally repeated close fails.
    // Existing read leases hold memory alive, but no new borrow can enter.
    bool close(Handle h, std::uint64_t owner) {
        std::lock_guard<std::mutex> guard(mutex_);
        Slot* slot = lookup(h);
        if (!slot || owner == 0 || slot->owner != owner || slot->phase != Phase::ready)
            return false;
        slot->phase = Phase::closing;
        if (slot->readers == 0) recycle(index(h));
        return true;
    }
    // Terminate a trusted actor incarnation. Already-published handles become
    // unreadable immediately. An in-progress ingress remains pinned until its
    // own move-only writer is dropped; publish fails once its owner is revoked.
    // Outstanding read leases may finish without risking recycled storage.
    std::size_t revoke_owner(std::uint64_t owner) noexcept {
        if (owner == 0) return 0;
        std::lock_guard<std::mutex> guard(mutex_);
        std::size_t revoked = 0;
        for (std::size_t i = 0; i < Slots; ++i) {
            auto& slot = slots_[i];
            if (slot.owner != owner) continue;
            if (slot.phase == Phase::writing) {
                slot.phase = Phase::aborting;
                ++revoked;
            } else if (slot.phase == Phase::ready) {
                slot.phase = Phase::closing;
                ++revoked;
                if (slot.readers == 0) recycle(i);
            }
        }
        return revoked;
    }
    std::size_t available() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return free_count_;
    }
};
} // namespace oreslang::runtime
