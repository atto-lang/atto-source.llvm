#pragma once
// Atto LLVM M2b candidate: bounded, actor-private, direct Offset32 heap.
// Trusted metadata lives outside guest-writable cage memory. This is NOT a
// generational Ref32 ABI; stale raw offsets still need compiler/handle fencing.
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime::vm {
class ActorSlabHeap32 final {
public:
    using Offset32 = std::uint32_t;
    static constexpr Offset32 kPageBytes = 4096;
    static constexpr Offset32 kMinClassBytes = 16;
    static constexpr Offset32 kMaxAlignment = 4096;
    static_assert(sizeof(Offset32) == 4, "guest offset must be four bytes");

    // The only host allocation is performed here, before actor admission.
    explicit ActorSlabHeap32(Offset32 capacity)
        : capacity_(capacity), pages_(static_cast<std::size_t>(
              (std::uint64_t{capacity} + kPageBytes - 1) / kPageBytes)) {}
    void bind(std::uint8_t* cage, Offset32 base) {
        if (!cage || base == 0 || std::uint64_t{base} + capacity_ >
                std::numeric_limits<Offset32>::max())
            throw std::invalid_argument("actor heap region exceeds Offset32 range");
        cage_ = cage;
        base_ = base;
    }
    Offset32 allocate(Offset32 bytes, Offset32 alignment) noexcept {
        if (!cage_ || !valid_request(bytes, alignment)) return 0;
        const std::uint64_t wanted = std::max<std::uint64_t>(bytes, alignment);
        std::uint64_t block = kMinClassBytes;
        while (block < wanted && block <= kPageBytes) block <<= 1;
        if (block <= kPageBytes) {
            for (std::size_t i = 0; i < pages_.size(); ++i) {
                if (pages_[i].mode == Mode::Slab && pages_[i].block == block) {
                    auto value = use_slab(i, static_cast<Offset32>(block));
                    if (value) return value;
                }
            }
            for (std::size_t i = 0; i < pages_.size(); ++i) {
                if (pages_[i].mode != Mode::Unused) continue;
                const auto first = aligned_first(i, static_cast<Offset32>(block));
                const auto end = page_end(i);
                if (first > end || block > end - first) continue;
                auto& page = pages_[i];
                page.mode = Mode::Slab;
                page.block = static_cast<Offset32>(block);
                page.slots = static_cast<std::uint16_t>((end - first) / block);
                return use_slab(i, static_cast<Offset32>(block));
            }
        }
        // Large runs are page-sized and tracked by their first page. An
        // unaligned actor base may need an extra page, never pointer wrap.
        for (std::size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].mode != Mode::Unused) continue;
            const auto start = align_up(page_begin(i), alignment);
            const auto end = start + bytes;
            if (end > std::uint64_t{base_} + capacity_) continue;
            const auto last = static_cast<std::size_t>((end - base_ + kPageBytes - 1) / kPageBytes);
            if (last <= i || last > pages_.size()) continue;
            bool unused = true;
            for (auto j = i; j < last; ++j) if (pages_[j].mode != Mode::Unused) { unused = false; break; }
            if (!unused) continue;
            pages_[i].mode = Mode::LargeHead;
            pages_[i].large_bytes = bytes;
            pages_[i].large_alignment = alignment;
            pages_[i].large_pages = static_cast<Offset32>(last - i);
            for (auto j = i + 1; j < last; ++j) pages_[j].mode = Mode::LargeTail;
            return static_cast<Offset32>(start);
        }
        return 0;
    }
    bool release(Offset32 offset, Offset32 bytes, Offset32 alignment) noexcept {
        if (!cage_ || !valid_request(bytes, alignment) || offset < base_ ||
                std::uint64_t{offset} >= std::uint64_t{base_} + capacity_ ||
                std::uint64_t{offset} + bytes > std::uint64_t{base_} + capacity_ ||
                offset % alignment != 0) return false;
        const auto i = static_cast<std::size_t>((offset - base_) / kPageBytes);
        auto& page = pages_[i];
        if (page.mode == Mode::Slab) {
            const auto first = aligned_first(i, page.block);
            if (offset < first || (offset - first) % page.block ||
                    bytes > page.block || alignment > page.block) return false;
            Offset32 expected = kMinClassBytes;
            while (expected < bytes || expected < alignment) {
                if (expected >= kPageBytes) return false;
                expected <<= 1;
            }
            if (expected != page.block) return false;
            const auto slot = static_cast<std::size_t>((offset - first) / page.block);
            if (slot >= page.slots || !get_bit(page, slot)) return false;
            clear_bit(page, slot);
            std::memset(cage_ + offset, 0, page.block);
            --page.active;
            if (!page.active) page = Page{};
            return true;
        }
        if (page.mode != Mode::LargeHead || page.large_bytes != bytes ||
                page.large_alignment != alignment ||
                offset != align_up(page_begin(i), alignment)) return false;
        const auto last = i + page.large_pages;
        if (last > pages_.size()) return false;
        for (auto j = i + 1; j < last; ++j)
            if (pages_[j].mode != Mode::LargeTail) return false;
        std::memset(cage_ + offset, 0, bytes);
        for (auto j = i; j < last; ++j) pages_[j] = Page{};
        return true;
    }
    // Only after all turns/loans have quiesced. Does not allocate or throw.
    void reset() noexcept {
        for (auto& page : pages_) page = Page{};
    }
    std::size_t metadata_bytes() const noexcept { return pages_.capacity() * sizeof(Page); }
    std::size_t page_count() const noexcept { return pages_.size(); }
    Offset32 capacity_bytes() const noexcept { return capacity_; }
private:
    enum class Mode : std::uint8_t { Unused, Slab, LargeHead, LargeTail };
    struct Page {
        std::array<std::uint64_t, 4> live{}; // 256 slots at 16 bytes min
        Offset32 block = 0, large_bytes = 0, large_alignment = 0, large_pages = 0;
        std::uint16_t slots = 0, active = 0;
        Mode mode = Mode::Unused;
    };
    static_assert(sizeof(Page) <= 64, "sidecar must remain compact");
    static bool valid_request(Offset32 bytes, Offset32 alignment) noexcept {
        return bytes != 0 && alignment != 0 && alignment <= kMaxAlignment &&
            (alignment & (alignment - 1)) == 0;
    }
    static std::uint64_t align_up(std::uint64_t v, Offset32 alignment) noexcept {
        return (v + alignment - 1) & ~std::uint64_t{alignment - 1};
    }
    std::uint64_t page_begin(std::size_t i) const noexcept {
        return std::uint64_t{base_} + std::uint64_t{i} * kPageBytes;
    }
    std::uint64_t page_end(std::size_t i) const noexcept {
        return page_begin(i) + std::min<std::uint64_t>(kPageBytes,
            capacity_ - std::uint64_t{i} * kPageBytes);
    }
    std::uint64_t aligned_first(std::size_t i, Offset32 block) const noexcept {
        return align_up(page_begin(i), block);
    }
    static bool get_bit(const Page& p, std::size_t i) noexcept {
        return (p.live[i / 64] >> (i % 64)) & 1u;
    }
    static void clear_bit(Page& p, std::size_t i) noexcept {
        p.live[i / 64] &= ~(std::uint64_t{1} << (i % 64));
    }
    Offset32 use_slab(std::size_t i, Offset32 block) noexcept {
        auto& page = pages_[i];
        if (page.active >= page.slots) return 0;
        for (std::size_t w = 0; w < page.live.size(); ++w) {
            const auto start = w * 64;
            if (start >= page.slots) break;
            const auto remaining = static_cast<unsigned>(page.slots - start);
            const std::uint64_t mask = remaining >= 64 ? ~std::uint64_t{0} :
                (std::uint64_t{1} << remaining) - 1;
            const std::uint64_t available = ~page.live[w] & mask;
            if (!available) continue;
            const auto bit = static_cast<unsigned>(__builtin_ctzll(available));
            const auto index = start + bit;
            page.live[w] |= std::uint64_t{1} << bit;
            ++page.active;
            const auto offset = aligned_first(i, block) + index * block;
            return static_cast<Offset32>(offset);
        }
        return 0;
    }
    Offset32 capacity_;
    Offset32 base_ = 0;
    std::uint8_t* cage_ = nullptr; // host pointer never guest-visible
    std::vector<Page> pages_; // allocated once, no allocations in alloc/release
};
}
