#include "oreslang/runtime/DopKernelIR.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
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
    // A kernel's output is reserved as one exclusive region, not a sequence
    // of independent per-element writes. Even other readers must fail closed.
    Plan batch_plan(Layout::AoSoA, 7, 3, 4, 16, 4);
    HostBuffer destination(batch_plan);
    {
        auto writer = destination.borrow_write();
        writer.write(0, 0, 19.0f);
        must_reject_loan([&] { (void)destination.read(0, 0); });
        must_reject_loan([&] { (void)destination.borrow_read(); });
        must_reject_loan([&] { (void)destination.borrow_write(); });
        must_reject_loan([&] { destination.write(1, 1, -1.0f); });
        must_reject_loan([&] { (void)destination.take(); });
        must_reject_loan([&] { destination.close(); });
        must_reject_loan([&] { HostBuffer stolen(std::move(destination)); });
        std::atomic<bool> rejected{false};
        std::thread observer([&] {
            try { (void)destination.read(0, 0); }
            catch (const std::logic_error&) { rejected.store(true, std::memory_order_release); }
        });
        observer.join();
        if (!rejected.load(std::memory_order_acquire))
            throw std::runtime_error("reader observed partially written kernel destination");
        writer.release();
        must_reject_loan([&] { writer.write(0, 0, 1.0f); });
    }
    if (destination.read(0, 0) != 19.0f)
        throw std::runtime_error("exclusive output loan did not commit writes");
    {
        auto reader = destination.borrow_read();
        must_reject_loan([&] { (void)destination.borrow_write(); });
    }
    destination.close();

    // Fail before the first output write if another CPU task holds the region;
    // its input read loan must unwind on failed output admission.
    HostBuffer batch_source(batch_plan), result(batch_plan);
    Kernel kernel(batch_plan, Element::F32, Op::ScaleBias, 2.0f, 1.0f);
    batch_source.write(0, 0, 7.0f);
    result.write(0, 0, 101.0f);
    {
        auto reserved = result.borrow_write();
        must_reject_loan([&] { execute_cpu(kernel, batch_source, result); });
        must_reject_loan([&] { (void)result.read(0, 0); });
    }
    auto source_successor = batch_source.take(); // batch_source read loan was released
    source_successor.close();
    if (result.read(0, 0) != 101.0f)
        throw std::runtime_error("rejected CPU kernel partially modified destination");
    result.close();

    // Direct C++ move/assignment must enforce the same active-loan
    // exclusion as the explicit take() method, without consuming the
    // source on a rejected transfer.
    Plan move_plan(Layout::AoS, 1, 1, 4, 4, 1);
    HostBuffer borrowed_source(move_plan), retired_target(move_plan);
    retired_target.close();
    {
        auto loan = borrowed_source.borrow_read();
        must_reject_loan([&] { HostBuffer stolen(std::move(borrowed_source)); });
        must_reject_loan([&] { retired_target = std::move(borrowed_source); });
        if (borrowed_source.layout().rows() != 1)
            throw std::runtime_error("failed native move consumed borrowed owner");
    }
    retired_target = std::move(borrowed_source);
    retired_target.close();
    HostBuffer closed_source(move_plan);
    closed_source.close();
    must_reject_loan([&] { HostBuffer bypass(std::move(closed_source)); });
    // Borrow admission and owner transfer contend on the *same wrapper*.
    // Exactly one wins. This used to race on HostBuffer::state_ itself
    // despite the shared backing State having an internal mutex.
    Plan scalar(Layout::AoS, 1, 1, 4, 4, 1);
    for (int trial = 0; trial < 128; ++trial) {
        HostBuffer contended(scalar);
        std::optional<HostBuffer::ReadLoan> borrowed;
        std::optional<HostBuffer> moved;
        std::exception_ptr borrow_error, move_error;
        std::atomic<bool> start{false};
        std::thread reader([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try { borrowed.emplace(contended.borrow_read()); }
            catch (const std::logic_error&) { }
            catch (...) { borrow_error = std::current_exception(); }
        });
        std::thread transfer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try { moved.emplace(contended.take()); }
            catch (const std::logic_error&) { }
            catch (...) { move_error = std::current_exception(); }
        });
        start.store(true, std::memory_order_release);
        reader.join();
        transfer.join();
        if (borrow_error) std::rethrow_exception(borrow_error);
        if (move_error) std::rethrow_exception(move_error);
        if (borrowed.has_value() == moved.has_value())
            throw std::runtime_error("borrow versus move must admit exactly one owner");
        if (borrowed) {
            must_reject_loan([&] { contended.close(); });
            borrowed.reset();
            contended.close();
        } else {
            moved->close();
            must_reject_loan([&] { (void)contended.layout(); });
        }
    }
    HostBuffer target(scalar), source(scalar);
    must_reject_loan([&] { target = std::move(source); });
    target.close();
    target = std::move(source);
    target.close();
    std::cout << "PASS: threaded read loans, exclusive ownership and quiescent transfer\n";
}
