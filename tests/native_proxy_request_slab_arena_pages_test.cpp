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
    slab.close();
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
    ores_actor_arena_leave(arena, turn);
    ores_actor_arena_retire(arena);
    assert(ores_actor_arena_try_destroy(arena));
    std::puts("NATIVE_PROXY_ARENA_PAGES_OK");
}
