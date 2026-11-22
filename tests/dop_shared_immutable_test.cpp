#include "oreslang/runtime/DopSharedImmutable.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace oreslang::dop;
static_assert(shared_immutable_cell_v<float> && shared_immutable_cell_v<std::int64_t>);
static_assert(!shared_immutable_cell_v<bool> && !shared_immutable_cell_v<int*> &&
              !shared_immutable_cell_v<std::vector<int>>);
static_assert(std::is_same_v<decltype(std::declval<PinnedImmutableView<int>>().span()),
                             std::span<const int>>);

static void check(bool ok) {
    if (!ok) throw std::runtime_error("shared immutable array assertion");
}
template<class E, class F> static void expect(F fn) {
    bool caught = false;
    try { fn(); } catch (const E&) { caught = true; }
    check(caught);
}
int main() {
    std::vector<std::int64_t> src{2, 4, 6, 8, 10};
    const auto* ptr = src.data();
    auto array = SharedImmutableArray<std::int64_t>::adopt(std::move(src));
    check(array.valid() && array.size() == 5);
    check(array.view().span().data() == ptr);  // move-adopt: ZERO element copies
    auto copy = array;
    check(copy.view().span().data() == ptr);   // Arc-style clone: ZERO element copies
    auto full = array.view();
    auto half = full.subview(1, 3);
    check(half.size() == 3 && half.at(0) == 4 && half.at(2) == 8);
    check(half.span().data() == ptr + 1);
    expect<std::out_of_range>([&]{ (void)full.subview(std::numeric_limits<std::size_t>::max(), 1); });
    expect<std::out_of_range>([&]{ (void)full.subview(4, 2); });
    expect<std::out_of_range>([&]{ (void)half.at(3); });

    auto weak = array.downgrade();
    // A view keeps the allocation alive independently of original handles.
    array = {};
    copy = {};
    check(!weak.expired() && weak.upgrade().has_value());
    full = {};
    check(half.at(1) == 6);
    half = {};
    check(weak.expired() && !weak.upgrade().has_value());

    auto base = SharedImmutableArray<int>::adopt(std::vector<int>(8192, 7));
    std::atomic<bool> good{true};
    std::vector<std::thread> threads;
    for (int i = 0; i < 12; ++i) {
        threads.emplace_back([&, local = base] {
            for (int j = 0; j < 1500; ++j) {
                auto pin = local.view(12, 4000);
                if (pin.at(3) != 7 || pin.span()[0] != 7)
                    good.store(false, std::memory_order_relaxed);
            }
        });
    }
    base = {};
    for (auto& t : threads) t.join();
    check(good.load());

    auto empty = SharedImmutableArray<int>::adopt({});
    check(empty.valid() && empty.size() == 0 && empty.view().span().empty());
    auto empty_sub = empty.view().subview(0, 0);
    check(empty_sub.empty());
}
