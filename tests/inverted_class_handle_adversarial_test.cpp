// Host-only inverted-class capability regression: second-field atomic rollback
// and concurrent slot recycling. No guest .atto or actor scheduler semantics.
#include "InvertedClassHandleContract.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

using Table = oreslang::dop::contract::InvertedClassHandleContract<4>;
static_assert(!std::is_default_constructible_v<Table::Handle>);
static_assert(!std::is_constructible_v<Table::Handle, void*>);
static_assert(!std::is_convertible_v<Table::Handle, void*>);

static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
template<class Error, class F>
static void rejects(F&& callback) {
    try { callback(); } catch (const Error&) { return; }
    throw std::runtime_error("expected fail-closed error");
}

int main() {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    Table table(507);
    auto h = table.push(17, max);
    // First column successfully computes 17+1; second column must fail.
    // Both columns must be unchanged, including after attempted underflow.
    rejects<std::overflow_error>([&] { table.move_by(h, 1, 1); });
    auto s = table.snapshot(h);
    check(s.x == 17 && s.y == max, "second-field overflow partially committed");
    table.move_by(h, 1, -1);
    s = table.snapshot(h);
    check(s.x == 18 && s.y == max - 1, "successful near-max update wrong");

    auto low = table.push(29, min);
    rejects<std::overflow_error>([&] { table.move_by(low, 1, -1); });
    s = table.snapshot(low);
    check(s.x == 29 && s.y == min, "second-field underflow partially committed");
    table.move_by(low, -1, 1);
    s = table.snapshot(low);
    check(s.x == 28 && s.y == min + 1, "successful near-min update wrong");

    // A stable surviving handle must not be redirected when another slot is
    // removed and repeatedly reused by a separate mutating native thread.
    Table churn(508);
    const auto retired = churn.push(1, 2);
    const auto survivor = churn.push(9, 10);
    std::atomic<bool> recycled{false};
    std::atomic<bool> bad{false};
    std::atomic<int> writer_progress{0};
    std::atomic<int> reader_progress{0};
    // Ensure neither thread can finish its entire loop before the other
    // has performed work: synchronize at three intermediate milestones.
    auto require_other = [](int i, const std::atomic<int>& progress) {
        if (i == 100 || i == 6000 || i == 11000)
            while (progress.load(std::memory_order_acquire) < i)
                std::this_thread::yield();
    };
    std::thread writer([&] {
        churn.erase_swap(retired);
        recycled.store(true, std::memory_order_release);
        for (int i = 0; i < 12000; ++i) {
            require_other(i, reader_progress);
            const auto temp = churn.push(i, -i);
            if (churn.snapshot(survivor).x != 9 ||
                churn.snapshot(survivor).y != 10)
                bad.store(true, std::memory_order_relaxed);
            churn.erase_swap(temp);
            writer_progress.store(i + 1, std::memory_order_release);
        }
    });
    std::thread reader([&] {
        while (!recycled.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < 12000; ++i) {
            require_other(i, writer_progress);
            try {
                (void)churn.snapshot(retired);
                bad.store(true, std::memory_order_relaxed);
            } catch (const std::out_of_range&) {
                // Expected: slot was invalidated before the release fence.
            } catch (...) {
                bad.store(true, std::memory_order_relaxed);
            }
            const auto actual = churn.snapshot(survivor);
            if (actual.x != 9 || actual.y != 10)
                bad.store(true, std::memory_order_relaxed);
            reader_progress.store(i + 1, std::memory_order_release);
        }
    });
    writer.join();
    reader.join();
    check(!bad.load(std::memory_order_relaxed),
          "stale identity revived or survivor corrupted under concurrent churn");
    rejects<std::out_of_range>([&] { (void)churn.snapshot(retired); });
    check(churn.size() == 1, "repeated slot recycling leaked live rows");
    return 0;
}
