#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/ProxyRequestSlab.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cstdio>
#include <span>

using oreslang::runtime::ProxyRequestSlab;

int main() {
    // 16,385 SoA rows -> (8 + 8) * 16,385 words = 1 MiB + 64 bytes.
    // This forces the real persistent arena API to cross a 1 MiB slot edge.
    constexpr std::uint32_t kCapacity = 16385;
    constexpr std::size_t kPageBytes = 1024U * 1024U;
    constexpr std::size_t kPageWords = kPageBytes / sizeof(std::uint32_t);
    const auto words = ProxyRequestSlab::required_words(
        ProxyRequestSlab::Layout::SoA, kCapacity, 1);
    assert(words * sizeof(std::uint32_t) == kPageBytes + 64);
    OresActorArena* arena = ores_actor_arena_open(7, 11, 2, kPageBytes, 1);
    // A valid live slot in a DIFFERENT arena is not an Oreslang slab page,
    // even if its slot index and generation happen to match locally.
    OresActorArena* other_arena =
        ores_actor_arena_open(9, 13, 1, kPageBytes, 1);
    const auto other_turn = ores_actor_arena_enter(other_arena, 9, 13);
    const auto real_foreign_handle =
        ores_actor_arena_alloc(other_arena, other_turn, kPageBytes);
    ores_actor_arena_leave(other_arena, other_turn);
    const auto turn = ores_actor_arena_enter(arena, 7, 11);
    std::array<OresArenaHandle, 2> pages = {
        ores_actor_arena_alloc(arena, turn, kPageBytes),
        ores_actor_arena_alloc(arena, turn, kPageBytes)
    };
    assert(ores_actor_arena_validate_pages(arena, turn,
        pages.data(), pages.size(), kPageBytes) == 1);
    assert(ores_actor_arena_validate_pages(arena, turn,
        pages.data(), pages.size(), kPageBytes - 4) == 0);
    assert(ores_actor_arena_validate_pages(arena, turn,
        pages.data(), 0, kPageBytes) == 0);
    const std::uint32_t secret = UINT32_C(0xDEADBEEF);
    // Contaminate both pages before constructing the guest's logical slab:
    // owner-enforced initialization must not leak prior arena bytes.
    ores_actor_arena_write(arena, turn, pages[0], 0, &secret, sizeof(secret));
    ores_actor_arena_write(arena, turn, pages[1], 60, &secret, sizeof(secret));
    ores_actor_arena_write(arena, turn, pages[1], kPageBytes - 4, &secret, sizeof(secret));
    // Invalid page aliases and surplus handles must fail before zeroing
    // any already-owned arena bytes.
    bool alias_rejected = false, extra_rejected = false;
    std::array<OresArenaHandle, 2> alias{pages[0], pages[0]};
    try {
        ProxyRequestSlab bad(ProxyRequestSlab::Layout::SoA, kCapacity, 1,
            arena, turn, alias, kPageWords);
    } catch (const std::invalid_argument&) { alias_rejected = true; }
    std::array<OresArenaHandle, 3> extra{pages[0], pages[1], pages[0]};
    try {
        ProxyRequestSlab bad(ProxyRequestSlab::Layout::SoA, kCapacity, 1,
            arena, turn, extra, kPageWords);
    } catch (const std::invalid_argument&) { extra_rejected = true; }
    assert(alias_rejected && extra_rejected);
    // Forged handles previously could reach a checked write only after
    // earlier valid pages had already been scrubbed. Preflight must instead
    // reject the whole vector before changing the first page.
    auto rejects_before_mutation = [&](OresArenaHandle forged) {
        std::array<OresArenaHandle, 2> proposed{pages[0], forged};
        bool denied = false;
        try {
            ProxyRequestSlab bad(ProxyRequestSlab::Layout::SoA, kCapacity, 1,
                arena, turn, proposed, kPageWords);
        } catch (const std::invalid_argument&) { denied = true; }
        assert(denied);
        std::uint32_t first_unchanged = 0;
        ores_actor_arena_read(arena, turn, pages[0], 0,
                              &first_unchanged, sizeof(first_unchanged));
        assert(first_unchanged == secret);
    };
    auto foreign = pages[1];
    ++foreign.domain;
    rejects_before_mutation(foreign);
    rejects_before_mutation(real_foreign_handle);
    auto stale = pages[1];
    ++stale.generation;
    rejects_before_mutation(stale);
    auto nonexistent = pages[1];
    nonexistent.index = 2;
    rejects_before_mutation(nonexistent);
    // Same slot with a changed generation is also an alias, not a new page.
    auto forged_alias = pages[0];
    ++forged_alias.generation;
    rejects_before_mutation(forged_alias);

    // Borrowed pages cannot enter a new slab claim. All-or-none failure
    // must keep the first page free and preserve precontaminated bytes.
    const auto loan = ores_actor_arena_borrow(arena, turn, pages[1]);
    bool borrowed_page_denied = false;
    try {
        ProxyRequestSlab bad(ProxyRequestSlab::Layout::SoA, kCapacity, 1,
            arena, turn, pages, kPageWords);
    } catch (const std::invalid_argument&) { borrowed_page_denied = true; }
    assert(borrowed_page_denied);
    assert(ores_actor_arena_claim_pages(
        arena, turn, pages.data(), pages.size(), kPageBytes) == 0);
    std::uint32_t borrowed_first = 0;
    ores_actor_arena_read(arena, turn, pages[0], 0,
                          &borrowed_first, sizeof(borrowed_first));
    assert(borrowed_first == secret);
    const auto independent_first = ores_actor_arena_claim_pages(
        arena, turn, pages.data(), 1, kPageBytes);
    assert(independent_first != 0);
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), 1, independent_first));
    ores_actor_arena_release(arena, loan);

    // A competing claim on the final page must also leave earlier pages
    // independently claimable, even when failure occurs late in the vector.
    const auto independent_second = ores_actor_arena_claim_pages(
        arena, turn, pages.data() + 1, 1, kPageBytes);
    assert(independent_second != 0);
    assert(ores_actor_arena_claim_pages(
        arena, turn, pages.data(), pages.size(), kPageBytes) == 0);
    const auto unclaimed_first = ores_actor_arena_claim_pages(
        arena, turn, pages.data(), 1, kPageBytes);
    assert(unclaimed_first != 0);
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), 1, unclaimed_first));
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data() + 1, 1, independent_second));

    std::uint32_t unchanged = 0;
    ores_actor_arena_read(arena, turn, pages[1], kPageBytes - 4,
                          &unchanged, sizeof(unchanged));
    assert(unchanged == secret); // invalid constructors did not zero pages.
    ProxyRequestSlab slab(ProxyRequestSlab::Layout::SoA, kCapacity, 1,
        arena, turn, pages, kPageWords);
    std::uint32_t first = 9, last = 9;
    ores_actor_arena_read(arena, turn, pages[0], 0, &first, sizeof(first));
    ores_actor_arena_read(arena, turn, pages[1], 60, &last, sizeof(last));
    std::uint32_t padded_tail = 9;
    ores_actor_arena_read(arena, turn, pages[1], kPageBytes - 4,
                          &padded_tail, sizeof(padded_tail));
    assert(first == 0 && last == 0 && padded_tail == 0);
    // Plant sentinels while the first slab holds its reservation:
    // failed competing constructors must not partially scrub either page.
    ores_actor_arena_write(arena, turn, pages[0], 0, &secret, sizeof(secret));
    ores_actor_arena_write(arena, turn, pages[1], kPageBytes - 4,
                          &secret, sizeof(secret));
    // Atomic claim prevents a second slab from zeroing even the *first*
    // page while this slab still owns both. Include subset overlap.
    auto rejects_claimed_pages = [&](std::span<const OresArenaHandle> claim_pages,
                                     std::uint32_t capacity) {
        bool denied = false;
        try {
            ProxyRequestSlab bad(ProxyRequestSlab::Layout::SoA, capacity, 1,
                arena, turn, claim_pages, kPageWords);
        } catch (const std::invalid_argument&) { denied = true; }
        assert(denied);
    };
    rejects_claimed_pages(pages, kCapacity);
    std::array<OresArenaHandle, 1> first_page{pages[0]};
    std::array<OresArenaHandle, 1> second_page{pages[1]};
    rejects_claimed_pages(first_page, 1);
    rejects_claimed_pages(second_page, 1);
    assert(ores_actor_arena_claim_pages(
        arena, turn, pages.data(), pages.size(), kPageBytes) == 0);
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), pages.size(), UINT64_MAX) == 0);
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), pages.size(), 0) == 0);
    assert(slab.live() == 0);
    std::uint32_t protected_first = 0, protected_last = 0;
    ores_actor_arena_read(arena, turn, pages[0], 0,
                          &protected_first, sizeof(protected_first));
    ores_actor_arena_read(arena, turn, pages[1], kPageBytes - 4,
                          &protected_last, sizeof(protected_last));
    assert(protected_first == secret && protected_last == secret);
    const std::uint32_t zero = 0;
    ores_actor_arena_write(arena, turn, pages[0], 0, &zero, sizeof(zero));


    // Caller-owned span may be changed or destroyed after construction.
    // The slab must use an immutable by-value handle snapshot instead.
    const auto saved_second_page = pages[1];
    pages[1] = pages[0];
    const auto copied_handle_ticket = slab.admit(1, 2, 3);
    pages[1] = saved_second_page;
    assert(copied_handle_ticket != 0);
    assert(slab.route(copied_handle_ticket) == 3);
    const auto copied_handle_claim = slab.pin(copied_handle_ticket);
    assert(copied_handle_claim != 0);
    assert(slab.retire(copied_handle_ticket));
    assert(slab.settle(copied_handle_claim));
    const auto ticket = slab.admit(1, 2, 3);
    assert(ticket != 0 && slab.route(ticket) == 3);
    const auto claim = slab.pin(ticket);
    assert(claim != 0);
    assert(slab.retire(ticket));
    assert(slab.settle(claim));
    assert(slab.live() == 0);

    // Inject a last-page secret after normal traffic. close() must zero all
    // logical words, including those at the last physical page boundary.
    ores_actor_arena_write(arena, turn, pages[1], 60, &secret, sizeof(secret));
    ores_actor_arena_write(arena, turn, pages[1], kPageBytes - 4, &secret, sizeof(secret));
    const auto closing_page = pages[1];
    pages[1] = pages[0]; // must not redirect the second-page close scrub
    slab.close();
    pages[1] = closing_page; // restore independent host probe handles
    std::array<std::uint32_t, 1024> check{};
    for (std::size_t off = 0; off < 2 * kPageWords;) {
        const auto page = off / kPageWords;
        const auto within = off % kPageWords;
        const auto count = std::min({2 * kPageWords - off, kPageWords - within, check.size()});
        ores_actor_arena_read(arena, turn, pages[page],
            within * sizeof(std::uint32_t), check.data(),
            count * sizeof(std::uint32_t));
        for (std::size_t i = 0; i < count; ++i) assert(check[i] == 0);
        off += count;
    }
    // Full close releases ownership. Distinct pages may now be safely
    // reused as two separate slabs, while overlap remains disallowed.
    {
        ProxyRequestSlab left(ProxyRequestSlab::Layout::SoA, 1, 1,
            arena, turn, first_page, kPageWords);
        ProxyRequestSlab right(ProxyRequestSlab::Layout::SoA, 1, 1,
            arena, turn, second_page, kPageWords);
        const auto left_ticket = left.admit(2, 3, 4);
        const auto right_ticket = right.admit(3, 4, 5);
        assert(left_ticket != 0 && right_ticket != 0);
        rejects_claimed_pages(first_page, 1);
        assert(left.retire(left_ticket));
        assert(right.retire(right_ticket));
        left.close();
        right.close();
        // Same physical slot can be reclaimed after real quiescence.
        ProxyRequestSlab retry(ProxyRequestSlab::Layout::SoA, 1, 1,
            arena, turn, first_page, kPageWords);
        retry.close();
    }
    // A multi-page claim must never be partially released, even by a
    // caller presenting the actual token and a valid first-page handle.
    const auto group_claim = ores_actor_arena_claim_pages(
        arena, turn, pages.data(), pages.size(), kPageBytes);
    assert(group_claim != 0);
    assert(!ores_actor_arena_unclaim_pages(
        arena, turn, first_page.data(), first_page.size(), group_claim));
    assert(ores_actor_arena_claim_pages(
        arena, turn, pages.data(), pages.size(), kPageBytes) == 0);
    assert(ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), pages.size(), group_claim));
    assert(!ores_actor_arena_unclaim_pages(
        arena, turn, pages.data(), pages.size(), group_claim));
    ores_actor_arena_leave(arena, turn);
    ores_actor_arena_retire(arena);
    assert(ores_actor_arena_try_destroy(arena));
    ores_actor_arena_retire(other_arena);
    assert(ores_actor_arena_try_destroy(other_arena));
    std::puts("NATIVE_PROXY_ARENA_PAGES_OK");
}
