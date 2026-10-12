#include "oreslang/runtime/DopKernelIR.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
template<class Fn> void must_reject_loan(Fn&& action) {
    try { action(); }
    catch (const std::logic_error&) { return; }
    throw std::runtime_error("mutating an outstanding borrowed buffer was admitted");
}
}

int main() {
    using namespace oreslang::dop;
    // Every loan is moved to a distinct reader thread. The owner stays on
    // this thread, but may neither write nor move/close until they drain.
    for (int iteration = 0; iteration < 16; ++iteration) {
        Plan layout(Layout::AoSoA, 7, 3, 4, 16, 4);
        HostBuffer owned(layout);
        for (std::uint64_t row = 0; row < layout.rows(); ++row)
            for (std::uint32_t field = 0; field < layout.fields(); ++field)
                owned.write(row, field, static_cast<float>(row * 3 + field));
        std::atomic<int> started{0};
        std::atomic<bool> proceed{false};
        std::atomic<bool> bad_read{false};
        std::vector<std::thread> readers;
        for (int reader = 0; reader < 4; ++reader) {
            auto loan = owned.borrow_read();
            readers.emplace_back([loan = std::move(loan), &started, &proceed, &bad_read]() mutable {
                started.fetch_add(1, std::memory_order_release);
                while (!proceed.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int trial = 0; trial < 64; ++trial) {
                    if (loan.read(6, 2) != 20.0f || loan.read(0, 0) != 0.0f)
                        bad_read.store(true, std::memory_order_relaxed);
                }
            });
        }
        while (started.load(std::memory_order_acquire) != 4) std::this_thread::yield();
        must_reject_loan([&] { owned.write(0, 0, -1.0f); });
        must_reject_loan([&] { (void)owned.take(); });
        must_reject_loan([&] { owned.close(); });
        proceed.store(true, std::memory_order_release);
        for (auto& thread : readers) thread.join();
        if (bad_read.load(std::memory_order_relaxed))
            throw std::runtime_error("shared read loan returned incorrect data");
        owned.write(0, 0, 42.0f);
        auto successor = owned.take();
        if (successor.read(0, 0) != 42.0f)
            throw std::runtime_error("ownership transfer lost a completed write");
        successor.close();
        must_reject_loan([&] { (void)successor.read(0, 0); });
    }
    std::cout << "PASS: threaded read loans, exclusive ownership and quiescent transfer\n";
}
