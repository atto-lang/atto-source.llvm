#pragma once

// Bounded, size-class slab allocator for one actor incarnation.
//
// Goal: make 32-bit actor references actually save memory. A 4-byte ref only
// helps if allocator metadata and fragmentation do not eat the saving, so:
//
//  * Objects carry no per-object header. Slab pages hold one size class each,
//    with a 64-byte in-page header (class, live count, page-local free list,
//    allocation bitmap): 1.56% of committed memory, zero host allocations.
//  * The region is reserved as address space and committed in granules on
//    demand, up to a budget. 32-bit hosts use small, realistic bounded heaps
//    rather than a large contiguous reservation.
//  * Offset32 (`CompressedActorRef`, offset/8 + 1) is the default reference:
//    no per-object metadata at all, for compiler-proven actor-local lifetimes.
//    It detects foreign, freed and misaligned refs (bitmap), not ABA.
//  * Handle32 is the explicit alternative where stale-reference protection
//    needs indirection: 8 bytes per handle-table entry, slot bits sized to the
//    configured capacity, FIFO slot reuse so generations wear evenly, and an
//    exhausted slot is quarantined (counted, never wrapped).
//
// Fast path (allocate/free/resolve of a small object with a partial page in
// its class): O(1), no syscalls, no host allocation. Slow paths: committing
// another granule (mprotect), first-fit search over free page runs (bounded by
// the page count). Every failure returns a status and leaves the heap
// unchanged. Single owner: the actor turn holding this heap. No locking.
//
// Not a sandbox and not a capability system: bounds, liveness and generation
// checks stop accidents and forged words, not hostile native code in-process.

#include "oreslang/runtime/ManagedRef.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define ORES_SLAB_HEAP_HAS_MMAP 1
#endif

namespace oreslang::runtime {

enum class SlabStatus : std::uint8_t {
    Ok,
    OutOfMemory,      // reservation exhausted
    BudgetExceeded,   // committing more would exceed the actor budget
    InvalidSize,
    InvalidAlignment, // > 64, or not a power of two
    InvalidRef,       // null, foreign, unallocated, interior or freed
    StaleHandle,      // Handle32 generation mismatch
    HandleTableFull,  // no free handle slot (live or quarantined)
    CommitFailed,     // the OS refused to commit
    Retired,
};

struct ActorSlabHeapConfig final {
    std::size_t reserve_bytes = 0;   // address-space span; never committed up front
    std::size_t budget_bytes = 0;    // max committed bytes, <= reserve
    std::size_t commit_granule = 64 * 1024;
    std::uint32_t handle_capacity = 0; // 0 = Offset32 only
    // Generation bits per Handle32 word; 0 = all bits not needed for the slot.
    // A smaller value is a deliberate workload profile (and makes exhaustion
    // testable); it never exceeds 32 - slot bits.
    std::uint32_t handle_generation_bits = 0;
};

struct ActorSlabHeapStats final {
    std::size_t reserved_bytes = 0;
    std::size_t committed_bytes = 0;
    std::size_t metadata_bytes = 0;  // page headers + handle table
    std::size_t live_block_bytes = 0; // size-class bytes of live objects
    std::size_t live_objects = 0;
    std::size_t live_pages = 0;      // small + large pages in use
    std::size_t free_pages = 0;      // carved pages returned to the pool
    std::uint32_t quarantined_handles = 0;
};

class ActorSlabHeap final {
public:
    using Ref = CompressedActorRef;
    struct Handle32 final {
        std::uint32_t word = 0;
        friend bool operator==(Handle32 a, Handle32 b) noexcept { return a.word == b.word; }
    };
    template<class T> struct Result final {
        SlabStatus status = SlabStatus::InvalidRef;
        T value{};
        explicit operator bool() const noexcept { return status == SlabStatus::Ok; }
    };

    static constexpr std::size_t kPageBytes = 4096;
    static constexpr std::size_t kHeaderBytes = 64;
    static constexpr std::size_t kMaxAlignment = 64;
    static constexpr std::array<std::uint16_t, 24> kClassBytes{
        16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
        320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048};
    static constexpr std::size_t kMaxSmall = 2048;
    static constexpr std::size_t kClasses = kClassBytes.size();

private:
    static constexpr std::uint32_t kNone = 0; // page links and free lists are index + 1
    // Page kinds live OUT of band: a large object's tail pages are payload,
    // so an in-page kind byte could be forged by guest data.
    enum PageKind : std::uint8_t { kUnused = 0, kSmall = 1, kLarge = 2, kLargeTail = 3, kFree = 4, kFreeTail = 5, kMeta = 6 };
    static constexpr std::size_t kBitmapBytes = kHeaderBytes - 16;

    // In-page header; lives at the start of every carved page. 64 bytes.
    struct PageHeader final {
        std::uint8_t reserved0; // kind is in kinds_, never in-page
        std::uint8_t cls;
        std::uint16_t live;      // live blocks (small)
        std::uint16_t free_head; // page-local free list, block index + 1
        std::uint16_t bump;      // blocks carved so far
        std::uint32_t prev;      // partial-list / free-run links (page + 1)
        std::uint32_t next;
        std::uint8_t bitmap[kBitmapBytes]; // small: bit set = allocated
    };
    static_assert(sizeof(PageHeader) == kHeaderBytes, "page header must be 64 bytes");
    static_assert(alignof(PageHeader) <= kHeaderBytes, "page header alignment");
    // Large runs reuse the bitmap bytes: run length in pages.
    static std::uint32_t& run_pages(PageHeader* h) noexcept {
        return *reinterpret_cast<std::uint32_t*>(h->bitmap);
    }

    static constexpr std::uint16_t blocks_in_page(std::size_t cls_bytes) noexcept {
        const std::size_t fit = (kPageBytes - kHeaderBytes) / cls_bytes;
        return static_cast<std::uint16_t>(fit < kBitmapBytes * 8 ? fit : kBitmapBytes * 8);
    }

    struct HandleEntry final {
        std::uint32_t ref;  // Offset32 word when live; next free slot + 1 when free
        std::uint32_t gen;  // current generation (1-based); 0 = quarantined
    };

    std::byte* base_ = nullptr;
    std::size_t reserve_ = 0;
    std::size_t budget_ = 0;
    std::size_t granule_ = 0;
    std::size_t committed_ = 0;
    bool owns_mapping_ = false;
    bool retired_ = false;

    std::uint32_t max_pages_ = 0;
    std::unique_ptr<std::uint8_t[]> kinds_; // one byte per page, allocated once
    std::uint32_t high_water_pages_ = 0; // pages ever carved
    std::array<std::uint32_t, kClasses> partial_{}; // class -> first partial page + 1
    std::uint32_t free_runs_ = kNone;   // free page runs (page + 1)

    // Handle32 table: a large run carved at construction, so it is counted.
    HandleEntry* handles_ = nullptr;
    std::uint32_t handle_capacity_ = 0;
    std::uint32_t slot_bits_ = 0;
    std::uint32_t max_gen_ = 0;
    std::uint32_t free_slot_head_ = kNone, free_slot_tail_ = kNone; // FIFO
    std::uint32_t quarantined_ = 0;

    ActorSlabHeapStats stats_{};

    static constexpr std::uint8_t class_for(std::size_t bytes, std::size_t align) noexcept {
        for (std::size_t c = 0; c < kClasses; ++c)
            if (kClassBytes[c] >= bytes && kClassBytes[c] % align == 0)
                return static_cast<std::uint8_t>(c);
        return static_cast<std::uint8_t>(kClasses);
    }

    PageHeader* page(std::uint32_t index) const noexcept {
        return reinterpret_cast<PageHeader*>(base_ + std::size_t{index} * kPageBytes);
    }
    std::size_t page_offset(std::uint32_t index) const noexcept {
        return std::size_t{index} * kPageBytes;
    }

    SlabStatus commit_through(std::size_t end) noexcept {
        if (end <= committed_) return SlabStatus::Ok;
        if (end > reserve_) return SlabStatus::OutOfMemory;
        std::size_t target = (end + granule_ - 1) / granule_ * granule_;
        if (target > reserve_) target = reserve_;
        if (target > budget_) {
            if (end > budget_) return SlabStatus::BudgetExceeded;
            target = budget_;
        }
#if ORES_SLAB_HEAP_HAS_MMAP
        if (owns_mapping_ && ::mprotect(base_ + committed_, target - committed_,
                                        PROT_READ | PROT_WRITE) != 0)
            return SlabStatus::CommitFailed;
#endif
        committed_ = target;
        stats_.committed_bytes = committed_;
        return SlabStatus::Ok;
    }

    // Take `count` contiguous pages: first fit over free runs, then fresh.
    Result<std::uint32_t> take_pages(std::uint32_t count) noexcept {
        for (std::uint32_t link = free_runs_; link != kNone;) {
            const std::uint32_t first = link - 1;
            PageHeader* h = page(first);
            const std::uint32_t have = run_pages(h);
            if (have >= count) {
                unlink_free_run(first);
                if (have > count) push_free_run(first + count, have - count);
                stats_.free_pages -= count;
                return {SlabStatus::Ok, first};
            }
            link = h->next;
        }
        if (count > max_pages_ - high_water_pages_) return {SlabStatus::OutOfMemory, 0};
        const std::uint32_t first = high_water_pages_;
        const SlabStatus s = commit_through(page_offset(first + count));
        if (s != SlabStatus::Ok) return {s, 0};
        high_water_pages_ += count;
        return {SlabStatus::Ok, first};
    }

    void push_free_run(std::uint32_t first, std::uint32_t count) noexcept {
        // Merge with the run directly after it, if that one is free.
        const std::uint32_t after = first + count;
        if (after < high_water_pages_ && kinds_[after] == kFree) {
            const std::uint32_t more = run_pages(page(after));
            unlink_free_run(after);
            count += more;
        }
        PageHeader* h = page(first);
        std::memset(h, 0, sizeof(PageHeader));
        kinds_[first] = kFree;
        for (std::uint32_t p = 1; p < count; ++p) kinds_[first + p] = kFreeTail;
        run_pages(h) = count;
        h->next = free_runs_;
        h->prev = kNone;
        if (free_runs_ != kNone) page(free_runs_ - 1)->prev = first + 1;
        free_runs_ = first + 1;
    }
    void unlink_free_run(std::uint32_t first) noexcept {
        PageHeader* h = page(first);
        if (h->prev != kNone) page(h->prev - 1)->next = h->next; else free_runs_ = h->next;
        if (h->next != kNone) page(h->next - 1)->prev = h->prev;
        kinds_[first] = kUnused; // caller re-labels every page it takes
    }

    void partial_push(std::uint8_t cls, std::uint32_t index) noexcept {
        PageHeader* h = page(index);
        h->prev = kNone;
        h->next = partial_[cls];
        if (partial_[cls] != kNone) page(partial_[cls] - 1)->prev = index + 1;
        partial_[cls] = index + 1;
    }
    void partial_remove(std::uint8_t cls, std::uint32_t index) noexcept {
        PageHeader* h = page(index);
        if (h->prev != kNone) page(h->prev - 1)->next = h->next; else partial_[cls] = h->next;
        if (h->next != kNone) page(h->next - 1)->prev = h->prev;
        h->prev = h->next = kNone;
    }

    static bool bit(const PageHeader* h, std::uint32_t i) noexcept {
        return (h->bitmap[i >> 3] >> (i & 7)) & 1u;
    }
    static void set_bit(PageHeader* h, std::uint32_t i, bool on) noexcept {
        const auto mask = static_cast<std::uint8_t>(1u << (i & 7));
        if (on) h->bitmap[i >> 3] = static_cast<std::uint8_t>(h->bitmap[i >> 3] | mask);
        else h->bitmap[i >> 3] = static_cast<std::uint8_t>(h->bitmap[i >> 3] & ~mask);
    }

    Result<Ref> encode(std::size_t offset) const noexcept {
        auto ref = Ref::encode(reinterpret_cast<std::uintptr_t>(base_), reserve_,
                               reinterpret_cast<std::uintptr_t>(base_ + offset));
        if (!ref) return {SlabStatus::InvalidRef, {}};
        return {SlabStatus::Ok, *ref};
    }

    struct Located final {
        std::uint32_t page;
        std::uint32_t block; // small: block index; large: run length in pages
        std::size_t bytes;   // usable bytes of the block
        bool large;
    };
    // Validate an Offset32 word: must name the start of a live block.
    Result<Located> locate(Ref ref) const noexcept {
        if (retired_) return {SlabStatus::Retired, {}};
        if (ref.is_null()) return {SlabStatus::InvalidRef, {}};
        const auto addr = ref.decode(reinterpret_cast<std::uintptr_t>(base_), reserve_);
        if (!addr) return {SlabStatus::InvalidRef, {}};
        const std::size_t offset = *addr - reinterpret_cast<std::uintptr_t>(base_);
        const auto index = static_cast<std::uint32_t>(offset / kPageBytes);
        const std::size_t within = offset % kPageBytes;
        if (index >= high_water_pages_ || within < kHeaderBytes) return {SlabStatus::InvalidRef, {}};
        const PageHeader* h = page(index);
        if (kinds_[index] == kSmall) {
            const std::size_t size = kClassBytes[h->cls];
            if ((within - kHeaderBytes) % size != 0) return {SlabStatus::InvalidRef, {}};
            const auto block = static_cast<std::uint32_t>((within - kHeaderBytes) / size);
            if (block >= h->bump || !bit(h, block)) return {SlabStatus::InvalidRef, {}};
            return {SlabStatus::Ok, {index, block, size, false}};
        }
        // kMeta (the handle table) is never a valid object, even at +64.
        if (kinds_[index] == kLarge && within == kHeaderBytes) {
            const std::uint32_t pages = run_pages(const_cast<PageHeader*>(h));
            return {SlabStatus::Ok, {index, pages, std::size_t{pages} * kPageBytes - kHeaderBytes, true}};
        }
        return {SlabStatus::InvalidRef, {}};
    }

    void init(std::byte* base, std::size_t reserve, const ActorSlabHeapConfig& config) {
        base_ = base;
        reserve_ = reserve;
        budget_ = config.budget_bytes;
        granule_ = config.commit_granule < kPageBytes ? kPageBytes
                 : config.commit_granule / kPageBytes * kPageBytes;
        max_pages_ = static_cast<std::uint32_t>(reserve / kPageBytes);
        stats_.reserved_bytes = reserve;
        // Construction-time host allocation (never on the fast path), counted.
        kinds_.reset(new (std::nothrow) std::uint8_t[max_pages_]());
        if (!kinds_) { retired_ = true; return; }
        stats_.metadata_bytes += max_pages_;
        if (config.handle_capacity == 0) return;
        handle_capacity_ = config.handle_capacity;
        while ((std::uint64_t{1} << slot_bits_) <= handle_capacity_) ++slot_bits_;
        if (slot_bits_ >= 31) { retired_ = true; return; } // need >= 2 generation bits
        std::uint32_t gen_bits = 32 - slot_bits_;
        if (config.handle_generation_bits != 0) {
            if (config.handle_generation_bits < 2 || config.handle_generation_bits > gen_bits) {
                retired_ = true;
                return;
            }
            gen_bits = config.handle_generation_bits;
        }
        max_gen_ = static_cast<std::uint32_t>((std::uint64_t{1} << gen_bits) - 1);
        const std::size_t table = std::size_t{handle_capacity_} * sizeof(HandleEntry);
        const auto pages = static_cast<std::uint32_t>((table + kHeaderBytes + kPageBytes - 1) / kPageBytes);
        const auto run = take_pages(pages);
        if (!run) { retired_ = true; return; }
        PageHeader* h = page(run.value);
        std::memset(h, 0, sizeof(PageHeader));
        kinds_[run.value] = kMeta;
        run_pages(h) = pages;
        for (std::uint32_t p = 1; p < pages; ++p) kinds_[run.value + p] = kLargeTail;
        handles_ = reinterpret_cast<HandleEntry*>(base_ + page_offset(run.value) + kHeaderBytes);
        for (std::uint32_t s = 0; s < handle_capacity_; ++s)
            handles_[s] = HandleEntry{s + 1 < handle_capacity_ ? s + 2 : kNone, 1};
        free_slot_head_ = 1;
        free_slot_tail_ = handle_capacity_;
        stats_.metadata_bytes += std::size_t{pages} * kPageBytes;
        stats_.live_pages += pages;
    }

public:
    // Reserve address space and commit on demand (POSIX). Fails closed: check ok().
    explicit ActorSlabHeap(const ActorSlabHeapConfig& config) {
        const std::size_t reserve = config.reserve_bytes / kPageBytes * kPageBytes;
        if (reserve == 0 || config.budget_bytes == 0 || config.budget_bytes > reserve
                || reserve > Ref::max_addressable_span
                || reserve / kPageBytes > std::numeric_limits<std::uint32_t>::max() - 1) {
            retired_ = true;
            return;
        }
#if ORES_SLAB_HEAP_HAS_MMAP
        void* mapping = ::mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON
#if defined(MAP_NORESERVE)
                               | MAP_NORESERVE
#endif
                               , -1, 0);
        if (mapping == MAP_FAILED) { retired_ = true; return; }
#if defined(MADV_NOHUGEPAGE)
        ::madvise(mapping, reserve, MADV_NOHUGEPAGE); // sparse commit; see pool
#endif
        owns_mapping_ = true;
        init(static_cast<std::byte*>(mapping), reserve, config);
#else
        retired_ = true; // use the caller-buffer constructor on this platform
#endif
    }

    // Caller-provided, already committed memory (embedded / no-MMU targets).
    ActorSlabHeap(void* buffer, std::size_t bytes, std::uint32_t handle_capacity = 0) {
        const auto addr = reinterpret_cast<std::uintptr_t>(buffer);
        const std::size_t reserve = bytes / kPageBytes * kPageBytes;
        if (!buffer || addr % kPageBytes != 0 || reserve == 0
                || reserve > Ref::max_addressable_span) {
            retired_ = true;
            return;
        }
        ActorSlabHeapConfig config{reserve, reserve, reserve, handle_capacity};
        init(static_cast<std::byte*>(buffer), reserve, config);
        if (!retired_) { committed_ = reserve; stats_.committed_bytes = reserve; }
        else return;
    }

    ActorSlabHeap(const ActorSlabHeap&) = delete;
    ActorSlabHeap& operator=(const ActorSlabHeap&) = delete;
    ~ActorSlabHeap() { retire(); }

    bool ok() const noexcept { return !retired_; }
    const ActorSlabHeapStats& stats() const noexcept { return stats_; }
    std::uint32_t handle_slot_bits() const noexcept { return slot_bits_; }
    std::uint32_t handle_max_generation() const noexcept { return max_gen_; }

    // ---- Offset32 ----------------------------------------------------------
    Result<Ref> allocate(std::size_t bytes, std::size_t align = 8) noexcept {
        if (retired_) return {SlabStatus::Retired, {}};
        if (bytes == 0) return {SlabStatus::InvalidSize, {}};
        if (align == 0 || (align & (align - 1)) || align > kMaxAlignment)
            return {SlabStatus::InvalidAlignment, {}};
        if (align < 8) align = 8; // ref32 quantum
        const std::uint8_t cls = bytes <= kMaxSmall ? class_for(bytes, align)
                                                    : static_cast<std::uint8_t>(kClasses);
        if (cls == kClasses) return allocate_large(bytes);

        std::uint32_t index;
        if (partial_[cls] != kNone) {
            index = partial_[cls] - 1;
        } else {
            const auto run = take_pages(1);
            if (!run) return {run.status, {}};
            index = run.value;
            PageHeader* h = page(index);
            std::memset(h, 0, sizeof(PageHeader));
            kinds_[index] = kSmall;
            h->cls = cls;
            partial_push(cls, index);
            stats_.metadata_bytes += kHeaderBytes;
            ++stats_.live_pages;
        }
        PageHeader* h = page(index);
        const std::size_t size = kClassBytes[cls];
        std::uint32_t block;
        if (h->free_head != 0) {
            block = h->free_head - 1u;
            std::uint16_t next;
            std::memcpy(&next, base_ + page_offset(index) + kHeaderBytes + block * size, sizeof(next));
            h->free_head = next;
        } else {
            block = h->bump;
            h->bump = static_cast<std::uint16_t>(h->bump + 1);
        }
        set_bit(h, block, true);
        h->live = static_cast<std::uint16_t>(h->live + 1);
        if (h->free_head == 0 && h->bump == blocks_in_page(size)) partial_remove(cls, index);
        const std::size_t offset = page_offset(index) + kHeaderBytes + block * size;
        std::memset(base_ + offset, 0, size);
        stats_.live_block_bytes += size;
        ++stats_.live_objects;
        return encode(offset);
    }

    SlabStatus free(Ref ref) noexcept {
        const auto at = locate(ref);
        if (!at) return at.status;
        const Located& l = at.value;
        PageHeader* h = page(l.page);
        if (l.large) {
            for (std::uint32_t p = 0; p < l.block; ++p)
                std::memset(base_ + page_offset(l.page + p) + (p == 0 ? kHeaderBytes : 0), 0,
                            p == 0 ? kPageBytes - kHeaderBytes : kPageBytes);
            push_free_run(l.page, l.block);
            stats_.free_pages += l.block;
            stats_.live_pages -= l.block;
            stats_.metadata_bytes -= kHeaderBytes;
            stats_.live_block_bytes -= l.bytes;
            --stats_.live_objects;
            return SlabStatus::Ok;
        }
        const std::size_t size = l.bytes;
        std::byte* block = base_ + page_offset(l.page) + kHeaderBytes + l.block * size;
        std::memset(block, 0, size); // never disclose a freed object's bytes
        const bool was_full = h->free_head == 0 && h->bump == blocks_in_page(size);
        set_bit(h, l.block, false);
        h->live = static_cast<std::uint16_t>(h->live - 1);
        stats_.live_block_bytes -= size;
        --stats_.live_objects;
        if (h->live == 0) {
            // Whole page free: give it back to every class, not just this one.
            if (!was_full) partial_remove(h->cls, l.page);
            push_free_run(l.page, 1);
            ++stats_.free_pages;
            --stats_.live_pages;
            stats_.metadata_bytes -= kHeaderBytes;
            return SlabStatus::Ok;
        }
        const std::uint16_t next = h->free_head;
        std::memcpy(block, &next, sizeof(next));
        h->free_head = static_cast<std::uint16_t>(l.block + 1);
        if (was_full) partial_push(h->cls, l.page);
        return SlabStatus::Ok;
    }

    // Checked dereference: `bytes` must fit inside the live block.
    Result<void*> resolve(Ref ref, std::size_t bytes) const noexcept {
        const auto at = locate(ref);
        if (!at) return {at.status, nullptr};
        if (bytes > at.value.bytes) return {SlabStatus::InvalidSize, nullptr};
        const auto addr = ref.decode(reinterpret_cast<std::uintptr_t>(base_), reserve_);
        return {SlabStatus::Ok, reinterpret_cast<void*>(*addr)};
    }

    // ---- Handle32 ----------------------------------------------------------
    Result<Handle32> allocate_handle(std::size_t bytes, std::size_t align = 8) noexcept {
        if (retired_) return {SlabStatus::Retired, {}};
        if (handle_capacity_ == 0 || free_slot_head_ == kNone) return {SlabStatus::HandleTableFull, {}};
        const auto obj = allocate(bytes, align);
        if (!obj) return {obj.status, {}};
        const std::uint32_t slot = free_slot_head_ - 1;
        HandleEntry& e = handles_[slot];
        free_slot_head_ = e.ref;
        if (free_slot_head_ == kNone) free_slot_tail_ = kNone;
        e.ref = obj.value.storage();
        return {SlabStatus::Ok, Handle32{(e.gen << slot_bits_) | (slot + 1)}};
    }

    Result<Ref> resolve_handle(Handle32 handle) const noexcept {
        if (retired_) return {SlabStatus::Retired, {}};
        const std::uint32_t mask = (std::uint32_t{1} << slot_bits_) - 1;
        const std::uint32_t slot1 = handle.word & mask;
        if (handle_capacity_ == 0 || slot1 == 0 || slot1 > handle_capacity_)
            return {SlabStatus::InvalidRef, {}};
        const HandleEntry& e = handles_[slot1 - 1];
        if (e.gen == 0 || (handle.word >> slot_bits_) != e.gen) return {SlabStatus::StaleHandle, {}};
        return {SlabStatus::Ok, Ref::from_storage(e.ref)};
    }

    SlabStatus free_handle(Handle32 handle) noexcept {
        const auto ref = resolve_handle(handle);
        if (!ref) return ref.status;
        const SlabStatus s = free(ref.value);
        if (s != SlabStatus::Ok) return s;
        const std::uint32_t slot = (handle.word & ((std::uint32_t{1} << slot_bits_) - 1)) - 1;
        HandleEntry& e = handles_[slot];
        if (e.gen == max_gen_) {
            e = HandleEntry{kNone, 0}; // quarantine: never wrap, never reuse
            ++quarantined_;
            stats_.quarantined_handles = quarantined_;
            return SlabStatus::Ok;
        }
        e = HandleEntry{kNone, e.gen + 1};
        // FIFO: append to the tail so every slot ages at the same rate.
        if (free_slot_tail_ != kNone) handles_[free_slot_tail_ - 1].ref = slot + 1;
        else free_slot_head_ = slot + 1;
        free_slot_tail_ = slot + 1;
        return SlabStatus::Ok;
    }

    // ---- Lifecycle ---------------------------------------------------------
    // Return free pages' physical memory to the OS (slow path, explicit).
    void trim() noexcept {
#if ORES_SLAB_HEAP_HAS_MMAP && defined(MADV_DONTNEED)
        if (retired_ || !owns_mapping_) return;
        for (std::uint32_t link = free_runs_; link != kNone;) {
            const std::uint32_t first = link - 1;
            PageHeader* h = page(first);
            const std::uint32_t pages = run_pages(h);
            link = h->next;
            if (pages > 1) // keep the header page that carries the run links
                ::madvise(base_ + page_offset(first + 1), std::size_t{pages - 1} * kPageBytes,
                          MADV_DONTNEED);
        }
#endif
    }

    // Actor STOP / incarnation end: scrub and release everything. Idempotent.
    void retire() noexcept {
        if (base_ == nullptr) { retired_ = true; return; }
        if (!owns_mapping_ && committed_ != 0) {
            volatile std::byte* p = base_;
            // Only pages ever carved can hold data; never fault in the rest.
            const std::size_t used = page_offset(high_water_pages_);
            for (std::size_t i = 0; i < used; ++i) p[i] = std::byte{0};
        }
#if ORES_SLAB_HEAP_HAS_MMAP
        if (owns_mapping_) ::munmap(base_, reserve_); // pages vanish; nothing to scrub
#endif
        base_ = nullptr;
        handles_ = nullptr;
        kinds_.reset();
        retired_ = true;
        committed_ = 0;
        stats_ = ActorSlabHeapStats{};
    }

private:
    Result<Ref> allocate_large(std::size_t bytes) noexcept {
        if (bytes > reserve_) return {SlabStatus::OutOfMemory, {}};
        const std::size_t total = bytes + kHeaderBytes;
        const auto pages = static_cast<std::uint32_t>((total + kPageBytes - 1) / kPageBytes);
        const auto run = take_pages(pages);
        if (!run) return {run.status, {}};
        PageHeader* h = page(run.value);
        std::memset(h, 0, sizeof(PageHeader));
        kinds_[run.value] = kLarge;
        run_pages(h) = pages;
        for (std::uint32_t p = 1; p < pages; ++p) kinds_[run.value + p] = kLargeTail;
        std::memset(base_ + page_offset(run.value) + kHeaderBytes, 0,
                    std::size_t{pages} * kPageBytes - kHeaderBytes);
        stats_.metadata_bytes += kHeaderBytes;
        stats_.live_pages += pages;
        stats_.live_block_bytes += std::size_t{pages} * kPageBytes - kHeaderBytes;
        ++stats_.live_objects;
        return encode(page_offset(run.value) + kHeaderBytes);
    }
};

// Many actors, one reservation. One mmap per actor costs a kernel mapping each
// (and mprotect commits split them further): 100k actors exceed Linux's
// default vm.max_map_count. The pool maps once; anonymous pages are demand-
// paged, so untouched slot bytes cost no RSS. Each slot backs one
// ActorSlabHeap through the caller-buffer constructor. Single owner (the
// scheduler); acquire/release are O(1) and never allocate.
class ActorHeapRegionPool final {
    std::byte* base_ = nullptr;
    std::size_t slot_bytes_ = 0;
    std::uint32_t slots_ = 0;
    std::unique_ptr<std::uint32_t[]> free_;
    // One byte per slot, allocated during construction. A duplicate STOP/
    // release must never publish the same slot twice for concurrent actors.
    std::unique_ptr<std::uint8_t[]> acquired_;
    std::uint32_t free_count_ = 0;

public:
    ActorHeapRegionPool(std::uint32_t slots, std::size_t slot_bytes) {
        slot_bytes_ = (slot_bytes + ActorSlabHeap::kPageBytes - 1)
                    / ActorSlabHeap::kPageBytes * ActorSlabHeap::kPageBytes;
        if (slots == 0 || slot_bytes_ == 0
                || slot_bytes_ > ActorSlabHeap::Ref::max_addressable_span
                || slot_bytes_ > std::numeric_limits<std::size_t>::max() / slots)
            return;
#if ORES_SLAB_HEAP_HAS_MMAP
        void* mapping = ::mmap(nullptr, slot_bytes_ * slots, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON
#if defined(MAP_NORESERVE)
                               | MAP_NORESERVE
#endif
                               , -1, 0);
        if (mapping == MAP_FAILED) return;
#if defined(MADV_NOHUGEPAGE)
        // Actor slots are sparse by design. With transparent huge pages one
        // touched 4 KiB page faults in 2 MiB, which measured ~3x RSS at 10k+
        // actors. Opt out; this is a hint and failure is harmless.
        ::madvise(mapping, slot_bytes_ * slots, MADV_NOHUGEPAGE);
#endif
        free_.reset(new (std::nothrow) std::uint32_t[slots]);
        acquired_.reset(new (std::nothrow) std::uint8_t[slots]());
        if (!free_ || !acquired_) {
            free_.reset();
            acquired_.reset();
            ::munmap(mapping, slot_bytes_ * slots);
            return;
        }
        base_ = static_cast<std::byte*>(mapping);
        slots_ = slots;
        for (std::uint32_t i = 0; i < slots; ++i) free_[i] = slots - 1 - i;
        free_count_ = slots;
#endif
    }
    ActorHeapRegionPool(const ActorHeapRegionPool&) = delete;
    ActorHeapRegionPool& operator=(const ActorHeapRegionPool&) = delete;
    ~ActorHeapRegionPool() {
#if ORES_SLAB_HEAP_HAS_MMAP
        if (base_) ::munmap(base_, slot_bytes_ * slots_);
#endif
    }

    bool ok() const noexcept { return base_ != nullptr; }
    std::size_t slot_bytes() const noexcept { return slot_bytes_; }
    std::uint32_t available() const noexcept { return free_count_; }

    void* acquire() noexcept {
        if (!base_ || free_count_ == 0) return nullptr;
        const std::uint32_t index = free_[free_count_ - 1];
        // Fail closed if free-list metadata becomes corrupt; never hand a
        // live actor's memory to another actor.
        if (index >= slots_ || acquired_[index] != 0) return nullptr;
        --free_count_;
        acquired_[index] = 1;
        return base_ + std::size_t{index} * slot_bytes_;
    }

    // Call after the slot's heap retired (which scrubbed its carved pages).
    bool release(void* slot) noexcept {
        if (!base_ || !slot || free_count_ == slots_) return false;
        // Integer-range validation avoids relational/subtraction operations
        // on an unrelated (possibly forged) pointer.
        const auto address = reinterpret_cast<std::uintptr_t>(slot);
        const auto base_address = reinterpret_cast<std::uintptr_t>(base_);
        if (address < base_address) return false;
        const std::size_t delta = static_cast<std::size_t>(address - base_address);
        if (delta >= slot_bytes_ * slots_ || delta % slot_bytes_ != 0) return false;
        const auto index = static_cast<std::uint32_t>(delta / slot_bytes_);
        if (acquired_[index] == 0) return false; // duplicate or never acquired
        acquired_[index] = 0;
#if ORES_SLAB_HEAP_HAS_MMAP && defined(MADV_DONTNEED)
        ::madvise(slot, slot_bytes_, MADV_DONTNEED); // give the RSS back
#endif
        free_[free_count_++] = index;
        return true;
    }
};

} // namespace oreslang::runtime
