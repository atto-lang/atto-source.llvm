#include "oreslang/runtime/DopKernelIR.hpp"

#include <atomic>
#include <exception>
#include <latch>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <typename A, typename B>
void race(A&& one, B&& two) {
    std::latch ready(2);
    std::atomic<bool> go{false};
    std::exception_ptr first_error, second_error;
    std::thread first([&] {
        ready.count_down();
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try { one(); } catch (...) { first_error = std::current_exception(); }
    });
    std::thread second([&] {
        ready.count_down();
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try { two(); } catch (...) { second_error = std::current_exception(); }
    });
    ready.wait();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    if (first_error) std::rethrow_exception(first_error);
    if (second_error) std::rethrow_exception(second_error);
}
}

int main() {
    using namespace oreslang::dop;
    const Plan shape(Layout::AoSoA, 7, 3, 4, 16, 4);
    for (int trial = 0; trial < 96; ++trial) {
        HostBuffer owner(shape);
        auto root = owner.region_root();
        std::optional<HostBuffer> successor;
        race(
            [&] {
                try { successor.emplace(owner.take()); }
                catch (const std::logic_error&) { /* close won */ }
            },
            [&] { owner.close(); });
        if (successor) {
            check(root.is_live_owned(), "moved-from close retired successor");
            check(root.same_root(successor->region_root()), "transfer minted another root");
            successor->write(0, 0, 42.0f);
            check(successor->read(0, 0) == 42.0f, "moved storage not accessible");
            successor->close();
        }
        check(!root.is_live_owned(), "close/take race left a live orphan root");
    }

    for (int trial = 0; trial < 96; ++trial) {
        HostBuffer owner(shape);
        auto root = owner.region_root();
        std::optional<HostBuffer::ReadLoan> loan;
        race(
            [&] {
                try { loan.emplace(owner.borrow_read()); }
                catch (const std::logic_error&) { /* close won */ }
            },
            [&] {
                try { owner.close(); }
                catch (const std::logic_error&) { /* read loan won */ }
            });
        if (loan) {
            check(root.is_live_owned(), "close retired allocation under read loan");
            check(loan->read(0, 0) == 0.0f, "admitted read loan could not read");
            loan.reset();
            owner.close();
        }
        check(!root.is_live_owned(), "close/read race left live root");
    }

    for (int trial = 0; trial < 96; ++trial) {
        HostBuffer owner(shape);
        auto root = owner.region_root();
        std::optional<HostBuffer::WriteLoan> loan;
        race(
            [&] {
                try { loan.emplace(owner.borrow_write()); }
                catch (const std::logic_error&) { /* close won */ }
            },
            [&] {
                try { owner.close(); }
                catch (const std::logic_error&) { /* write loan won */ }
            });
        if (loan) {
            check(root.is_live_owned(), "close retired allocation under write loan");
            loan->write(1, 2, 17.0f);
            loan.reset();
            check(owner.read(1, 2) == 17.0f, "exclusive write lost data");
            owner.close();
        }
        check(!root.is_live_owned(), "close/write race left live root");
    }
}
