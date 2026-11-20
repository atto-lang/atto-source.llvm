#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

// Contended-counter microbenchmark, not an end-to-end scheduler speed claim.
// Compare equal logical fields; cold handles stay untouched in each layout.
struct WorkerAoS {
    std::thread handle;
    std::atomic<std::uint8_t> state{0};
    std::atomic<std::uint64_t> completed{0};
};
template<std::size_t Stride> struct WorkerSoA {
    std::array<std::thread, 4> handles{};
    std::array<std::atomic<std::uint8_t>, 4 * 64> states{};
    alignas(64) std::array<std::atomic<std::uint64_t>, 4 * Stride> completed{};
};
template<class Counter> double measure(unsigned workers, Counter counter) {
    constexpr unsigned updates = 250000;
    std::atomic<unsigned> ready{0}; std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back([&, i] {
        ++ready; while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned j = 0; j < updates; ++j) counter(i).fetch_add(1, std::memory_order_relaxed);
    });
    while (ready != workers) std::this_thread::yield();
    const auto begin = std::chrono::steady_clock::now(); start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count()
        / (workers * updates);
}
int main() {
    for (unsigned workers = 1; workers <= 4; ++workers) {
        std::array<double, 5> aos{}, dense{}, padded{};
        for (unsigned run = 0; run < 5; ++run) {
            std::array<WorkerAoS, 4> rows{};
            WorkerSoA<1> compact; WorkerSoA<8> separate;
            aos[run] = measure(workers, [&](unsigned i) -> auto& { return rows[i].completed; });
            dense[run] = measure(workers, [&](unsigned i) -> auto& { return compact.completed[i]; });
            padded[run] = measure(workers, [&](unsigned i) -> auto& { return separate.completed[i * 8]; });
        }
        std::sort(aos.begin(), aos.end()); std::sort(dense.begin(), dense.end()); std::sort(padded.begin(), padded.end());
        std::cout << "{\"workers\":" << workers << ",\"median_ns_per_counter_update\":{\"aos\":"
            << aos[2] << ",\"dense_soa\":" << dense[2] << ",\"padded_soa\":" << padded[2] << "}}\n";
    }
}
