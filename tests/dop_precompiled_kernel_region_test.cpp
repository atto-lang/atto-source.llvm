#include "oreslang/runtime/DopPrecompiledKernelRegion.hpp"

#include <cassert>
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

using namespace oreslang::dop;

// Count the C++ allocation boundary on the admitted read/write path only.
// CheckedRegion allocation during construction / explicit relayout is allowed.
static std::atomic<std::uint64_t> host_new_calls{0};
void* operator new(std::size_t size) {
    host_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    host_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }


static_assert(!std::is_default_constructible_v<DopPrecompiledKernelRegion::Ticket>);
static_assert(!std::is_copy_constructible_v<DopPrecompiledKernelRegion>);
static_assert(!std::is_move_constructible_v<DopPrecompiledKernelRegion>);

template<typename E, typename F>
void rejects(F&& task) {
    bool did_reject = false;
    try { task(); } catch (const E&) { did_reject = true; }
    assert(did_reject);
}

static Plan plan(Layout layout, std::uint64_t rows = 17, std::uint32_t fields = 3) {
    return Plan(layout, rows, fields, 4, 8, layout == Layout::AoSoA ? 4 : 1);
}

void positive_matrix() {
    for (Layout initial : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        DopPrecompiledKernelRegion region(plan(initial), false);
        assert(region.layout() == initial);
        auto token = region.ticket();
        for (std::uint64_t row = 0; row != region.rows(); ++row) {
            const auto handle = region.row(row);
            for (std::uint32_t field = 0; field != region.fields(); ++field)
                region.write(token, handle, field,
                             static_cast<std::uint32_t>(0x10000 + row * 101 + field));
        }
        for (Layout next : {Layout::SoA, Layout::AoSoA, Layout::AoS,
                            Layout::AoSoA, Layout::SoA, Layout::AoS}) {
            const auto stale_token = region.ticket();
            const auto stale_handle = region.row(3);
            const auto before = region.generation();
            region.relayout(plan(next));
            assert(region.generation() == before + 1);
            assert(region.layout() == next);
            rejects<std::logic_error>([&] {
                (void)region.read(stale_token, region.row(3), 0);
            });
            rejects<std::invalid_argument>([&] {
                (void)region.read(region.ticket(), stale_handle, 0);
            });
            token = region.ticket();
            for (std::uint64_t row = 0; row != region.rows(); ++row)
                for (std::uint32_t field = 0; field != region.fields(); ++field)
                    assert(region.read(token, region.row(row), field) ==
                           static_cast<std::uint32_t>(0x10000 + row * 101 + field));
        }
        // A failed structural migration cannot retire valid tickets/rows.
        const auto stable_token = region.ticket();
        const auto stable_row = region.row(16);
        const auto gen = region.generation();
        rejects<std::invalid_argument>([&] { region.relayout(plan(Layout::AoS, 18)); });
        rejects<std::invalid_argument>([&] { region.relayout(plan(Layout::AoS, 17, 4)); });
        rejects<std::invalid_argument>([&] {
            region.relayout(Plan(Layout::SoA, 17, 3, 8, 8, 1));
        });
        assert(region.generation() == gen);
        assert(region.read(stable_token, stable_row, 0) == 0x10000 + 16 * 101);
    }
}

void negative_security() {
    DopPrecompiledKernelRegion first(plan(Layout::AoS), false);
    DopPrecompiledKernelRegion second(plan(Layout::AoS), false);
    const auto first_ticket = first.ticket();
    const auto second_ticket = second.ticket();
    const auto first_row = first.row(0);
    const auto second_row = second.row(0);
    rejects<std::logic_error>([&] { (void)second.read(first_ticket, second_row, 0); });
    rejects<std::logic_error>([&] { second.write(first_ticket, second_row, 0, 4); });
    rejects<std::invalid_argument>([&] { (void)second.read(second_ticket, first_row, 0); });
    rejects<std::out_of_range>([&] { (void)first.row(17); });
    rejects<std::out_of_range>([&] { (void)first.read(first_ticket, first_row, 3); });
    rejects<std::out_of_range>([&] { first.write(first_ticket, first_row, 3, 10); });
    assert(first.read(first_ticket, first_row, 0) == 0);

    DopPrecompiledKernelRegion sealed(plan(Layout::AoSoA));
    assert(sealed.sealed());
    const auto original_token = sealed.ticket();
    const auto original_row = sealed.row(0);
    rejects<std::logic_error>([&] { sealed.relayout(plan(Layout::SoA)); });
    assert(sealed.generation() == 1);
    sealed.write(original_token, original_row, 0, 42);
    assert(sealed.read(original_token, original_row, 0) == 42);

    // Unsupported kernel width is refused, rather than narrowed/widened.
    rejects<std::invalid_argument>([] {
        DopPrecompiledKernelRegion bad(Plan(Layout::AoS, 1, 1, 8, 8, 1));
    });
}

void allocation_free_hot_path() {
    DopPrecompiledKernelRegion region(plan(Layout::AoSoA));
    const auto ticket = region.ticket();
    const auto row = region.row(8);
    const auto before = host_new_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 10000; ++i) {
        region.write(ticket, row, 1, static_cast<std::uint32_t>(i));
        assert(region.read(ticket, row, 1) == static_cast<std::uint32_t>(i));
    }
    assert(host_new_calls.load(std::memory_order_relaxed) == before);
}

int main() {
    positive_matrix();
    negative_security();
    allocation_free_hot_path();
}
