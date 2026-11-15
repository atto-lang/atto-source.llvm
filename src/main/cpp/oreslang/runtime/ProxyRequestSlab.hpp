#pragma once

#include "oreslang/runtime/ActorArena.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace oreslang::runtime {

// Actor-owned, externally allocated native request columns. The owning actor
// arena MUST outlive this borrowed slab. Only one logical actor turn may touch
// the memory; this host-only type has no scheduler/guest authority by itself.
// Reclaim/cancel callbacks must be routed to the owner turn before invoking it.
class ProxyRequestSlab final {
public:
    enum class Layout { SoA, AoSoA };
    static constexpr std::uint32_t kPins = 8;
    static constexpr std::uint32_t kFields = 8;
    static constexpr std::uint32_t kNoSlot = UINT32_MAX;
    static constexpr std::uint32_t kLimit = INT32_MAX;

    static std::size_t required_words(Layout layout, std::uint32_t capacity,
                                      std::uint32_t lanes) {
        // Reject forged/invalid enum discriminants before using lane math.
        // Otherwise an unknown layout with lanes=0 reaches row/lanes in
        // offset() and can trigger undefined behavior in the host runtime.
        if ((layout != Layout::SoA && layout != Layout::AoSoA)
            || capacity == 0 || capacity > 65536
            || (layout == Layout::SoA && lanes != 1)
            || (layout == Layout::AoSoA && lanes != 16 && lanes != 32 && lanes != 64)) {
            throw std::invalid_argument("invalid native proxy capacity/tile width");
        }
        const std::size_t rows = layout == Layout::AoSoA
            ? ((capacity + lanes - 1) / lanes) * lanes : capacity;
        const std::size_t count = rows * kFields + capacity * kPins;
        if (count > (16U * 1024U * 1024U) / sizeof(std::uint32_t)) {
            throw std::length_error("native proxy slab exceeds 16MiB");
        }
        return count;
    }

    ProxyRequestSlab(Layout layout, std::uint32_t capacity, std::uint32_t lanes,
                     std::span<std::uint32_t> memory)
        : layout_(layout), capacity_(capacity), lanes_(lanes), words_(memory) {
        if (words_.size() != required_words(layout, capacity, lanes)) {
            throw std::invalid_argument("native proxy backing span size mismatch");
        }
        rows_ = layout_ == Layout::AoSoA
            ? ((capacity_ + lanes_ - 1) / lanes_) * lanes_ : capacity_;
        receipt_base_ = rows_ * kFields;
        std::fill(words_.begin(), words_.end(), 0);
        free_head_ = 0;
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            put(i, kNextFree, i + 1 == capacity_ ? kNoSlot : i + 1);
        }
    }
    // Authoritative storage lives inside generation-checked persistent arena
    // slots, not in an external vector. A trusted owner adapter must enter a
    // valid arena turn, then resume() once per mailbox turn before invoking
    // operations; a forged/stale turn fails closed at the C arena boundary.
    ProxyRequestSlab(Layout layout, std::uint32_t capacity, std::uint32_t lanes,
                     OresActorArena* arena, OresArenaTurn turn,
                     std::span<const OresArenaHandle> pages,
                     std::size_t page_words)
        : layout_(layout), capacity_(capacity), lanes_(lanes),
          arena_(arena), turn_(turn), pages_(pages), page_words_(page_words) {
        const auto count = required_words(layout, capacity, lanes);
        if (!arena_ || pages_.empty() || pages_.size() > owned_pages_.size()
                || page_words_ == 0
                || page_words_ > (1024U * 1024U) / sizeof(std::uint32_t)
                || pages_.size() != (count + page_words_ - 1) / page_words_) {
            throw std::invalid_argument("invalid arena-backed proxy pages");
        }
        std::copy(pages_.begin(), pages_.end(), owned_pages_.begin());
        pages_ = std::span<const OresArenaHandle>(owned_pages_.data(), pages_.size());
        // Atomic all-pages reservation: two logical slabs cannot initialize
        // overlapping live arena slots. A failed attempt makes no writes.
        // Close releases only after all request receipts settle and pages
        // are physically scrubbed; host destruction cannot skip this receipt.
        page_claim_ = ores_actor_arena_claim_pages(arena_, turn_, pages_.data(),
                pages_.size(), page_words_ * sizeof(std::uint32_t));
        if (!page_claim_) {
            throw std::invalid_argument("proxy pages invalid or already claimed");
        }
        rows_ = layout_ == Layout::AoSoA
            ? ((capacity_ + lanes_ - 1) / lanes_) * lanes_ : capacity_;
        receipt_base_ = rows_ * kFields;
        zero_words();
        free_head_ = 0;
        for (std::uint32_t row = 0; row < capacity_; ++row) {
            put(row, kNextFree, row + 1 == capacity_ ? kNoSlot : row + 1);
        }
    }

    void resume(OresArenaTurn turn) {
        if (!arena_) throw std::logic_error("not arena-backed");
        turn_ = turn; // The arena checks domain, epoch and bound carrier.
    }

    ProxyRequestSlab(const ProxyRequestSlab&) = delete;
    ProxyRequestSlab& operator=(const ProxyRequestSlab&) = delete;
    ProxyRequestSlab(ProxyRequestSlab&&) = delete;
    ProxyRequestSlab& operator=(ProxyRequestSlab&&) = delete;

    std::uint32_t live() const { check_open(); return live_; }
    std::uint32_t capacity() const noexcept { return capacity_; }
    bool full() const { check_open(); return free_head_ == kNoSlot; }

    // O(1) admission, no allocation, 0 means bounded backpressure.
    std::uint64_t admit(std::uint32_t epoch, std::uint32_t connection,
                        std::uint32_t route) {
        check_open();
        if (epoch == 0 || epoch > kLimit || connection > kLimit || route > kLimit) {
            throw std::invalid_argument("invalid request metadata");
        }
        if (free_head_ == kNoSlot) return 0;
        const auto row = free_head_;
        free_head_ = get(row, kNextFree);
        const auto incarnation = get(row, kIncarnation) + 1;
        put(row, kIncarnation, incarnation);
        put(row, kEpoch, epoch);
        put(row, kConnection, connection);
        put(row, kRoute, route);
        put(row, kPhase, kOpen);
        put(row, kPending, 0);
        put(row, kNextFree, kNoSlot);
        ++live_;
        return (std::uint64_t{incarnation} << 32U) | row;
    }

    std::uint32_t epoch(std::uint64_t ticket) const {
        check_open(); return get(checked_row(ticket), kEpoch);
    }
    std::uint32_t route(std::uint64_t ticket) const {
        check_open(); return get(checked_row(ticket), kRoute);
    }
    std::uint32_t connection(std::uint64_t ticket) const {
        check_open(); return get(checked_row(ticket), kConnection);
    }
    std::uint32_t pending(std::uint64_t ticket) const {
        check_open(); return get(checked_row(ticket), kPending);
    }

    // O(8) bounded claim allocation, one serial per active receipt lane.
    std::uint64_t pin(std::uint64_t ticket) {
        check_open();
        const auto row = checked_row(ticket);
        if (get(row, kPhase) != kOpen) throw std::logic_error("request retiring");
        auto serial = get(row, kSerial);
        if (serial == kLimit) throw std::overflow_error("pin serial exhausted");
        for (std::uint32_t i = 0; i < kPins; ++i) {
            if (receipt(row, i) == 0) {
                ++serial;
                put_receipt(row, i, serial);
                put(row, kSerial, serial);
                put(row, kPending, get(row, kPending) + 1);
                const auto index = row * kPins + i + 1;
                return (std::uint64_t{serial} << 32U) | index;
            }
        }
        return 0;
    }

    // A receipt is spent precisely once; replay/ABA cannot free a new owner.
    bool settle(std::uint64_t claim) {
        check_open();
        const auto serial = static_cast<std::uint32_t>(claim >> 32U);
        const auto index = static_cast<std::uint32_t>(claim);
        if (serial == 0 || serial > kLimit || index == 0
                || index > capacity_ * kPins) return false;
        const auto row = (index - 1) / kPins;
        const auto lane = (index - 1) % kPins;
        const auto phase = get(row, kPhase);
        if ((phase != kOpen && phase != kRetiring) || receipt(row, lane) != serial) {
            return false;
        }
        put_receipt(row, lane, 0);
        const auto remaining = get(row, kPending) - 1;
        put(row, kPending, remaining);
        if (phase == kRetiring && remaining == 0) recycle(row);
        return true;
    }

    bool retire(std::uint64_t ticket) {
        check_open();
        const auto row = static_cast<std::uint32_t>(ticket);
        const auto incarnation = static_cast<std::uint32_t>(ticket >> 32U);
        if (row >= capacity_ || incarnation == 0 || incarnation > kLimit
                || get(row, kIncarnation) != incarnation) return false;
        const auto phase = get(row, kPhase);
        if (phase == kRetiring) return true;
        if (phase != kOpen) return false;
        put(row, kPhase, kRetiring);
        if (get(row, kPending) == 0) recycle(row);
        return true;
    }

    // An actor must drain all external I/O pins before closing the slab.
    // Does not free arena-owned backing memory or grant authorization.
    void close() {
        check_open();
        if (live_ != 0) throw std::logic_error("unretired native requests");
        zero_words();
        if (arena_) {
            if (!page_claim_ || !ores_actor_arena_unclaim_pages(
                    arena_, turn_, pages_.data(), pages_.size(), page_claim_)) {
                throw std::logic_error("proxy slab page claim release failed");
            }
            page_claim_ = 0;
        }
        closed_ = true;
        free_head_ = kNoSlot;
    }

private:
    static constexpr std::uint32_t kIncarnation = 0, kEpoch = 1;
    static constexpr std::uint32_t kConnection = 2, kRoute = 3;
    static constexpr std::uint32_t kPhase = 4, kPending = 5;
    static constexpr std::uint32_t kNextFree = 6, kSerial = 7;
    static constexpr std::uint32_t kFree = 0, kOpen = 1;
    static constexpr std::uint32_t kRetiring = 2, kExhausted = 3;

    void check_open() const {
        if (closed_) throw std::logic_error("native proxy slab is closed");
    }
    std::size_t offset(std::uint32_t row, std::uint32_t field) const noexcept {
        return layout_ == Layout::SoA ? static_cast<std::size_t>(field) * rows_ + row
            : (static_cast<std::size_t>(row / lanes_) * kFields + field) * lanes_
                + (row % lanes_);
    }
    // Bulk scrub through the existing checked arena API. This retains
    // owner-turn and carrier verification on every write, without taking
    // hundreds of thousands of spinlock acquisitions for individual words.
    // Arena pages may contain prior logical state, so never assume calloc.
    void zero_words() {
        if (!arena_) {
            std::fill(words_.begin(), words_.end(), 0);
            return;
        }
        constexpr std::size_t kChunkBytes = 4096;
        constexpr std::size_t kChunkWords = kChunkBytes / sizeof(std::uint32_t);
        const std::array<std::uint32_t, kChunkWords> zero{};
        std::size_t offset = 0;
        while (offset < pages_.size() * page_words_) {
            const auto page = offset / page_words_;
            const auto within = offset % page_words_;
            const auto extent = std::min({pages_.size() * page_words_ - offset,
                page_words_ - within, kChunkWords});
            ores_actor_arena_write(arena_, turn_, pages_[page],
                within * sizeof(std::uint32_t), zero.data(),
                extent * sizeof(std::uint32_t));
            offset += extent;
        }
    }

    std::uint32_t read_word(std::size_t word) const {
        if (!arena_) return words_[word];
        const auto page = word / page_words_;
        const auto byte_offset = (word % page_words_) * sizeof(std::uint32_t);
        std::uint32_t value = 0;
        ores_actor_arena_read(arena_, turn_, pages_[page], byte_offset,
                              &value, sizeof(value));
        return value;
    }
    void write_word(std::size_t word, std::uint32_t value) {
        if (!arena_) {
            words_[word] = value;
            return;
        }
        const auto page = word / page_words_;
        const auto byte_offset = (word % page_words_) * sizeof(std::uint32_t);
        ores_actor_arena_write(arena_, turn_, pages_[page], byte_offset,
                               &value, sizeof(value));
    }
    std::uint32_t get(std::uint32_t row, std::uint32_t field) const {
        return read_word(offset(row, field));
    }
    void put(std::uint32_t row, std::uint32_t field, std::uint32_t value) {
        write_word(offset(row, field), value);
    }
    std::uint32_t receipt(std::uint32_t row, std::uint32_t index) const {
        return read_word(receipt_base_ + row * kPins + index);
    }
    void put_receipt(std::uint32_t row, std::uint32_t index, std::uint32_t value) {
        write_word(receipt_base_ + row * kPins + index, value);
    }
    std::uint32_t checked_row(std::uint64_t ticket) const {
        const auto row = static_cast<std::uint32_t>(ticket);
        const auto incarnation = static_cast<std::uint32_t>(ticket >> 32U);
        if (row >= capacity_ || incarnation == 0 || incarnation > kLimit
                || get(row, kIncarnation) != incarnation
                || get(row, kPhase) == kFree || get(row, kPhase) == kExhausted) {
            throw std::logic_error("stale native proxy request ticket");
        }
        return row;
    }
    void recycle(std::uint32_t row) noexcept {
        put(row, kConnection, 0);
        put(row, kRoute, 0);
        put(row, kEpoch, 0);
        --live_;
        if (get(row, kIncarnation) == kLimit || get(row, kSerial) == kLimit) {
            put(row, kPhase, kExhausted);
        } else {
            put(row, kPhase, kFree);
            put(row, kNextFree, free_head_);
            free_head_ = row;
        }
    }

    Layout layout_;
    std::uint32_t capacity_;
    std::uint32_t lanes_;
    std::uint32_t rows_ = 0;
    std::size_t receipt_base_ = 0;
    std::span<std::uint32_t> words_;
    OresActorArena* arena_ = nullptr;
    OresArenaTurn turn_{};
    std::span<const OresArenaHandle> pages_{};
    std::array<OresArenaHandle, 16> owned_pages_{};
    std::size_t page_words_ = 0;
    std::uint64_t page_claim_ = 0;
    std::uint32_t free_head_ = kNoSlot;
    std::uint32_t live_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::runtime
