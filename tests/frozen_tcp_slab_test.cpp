#include "oreslang/runtime/FrozenTcpSlab.hpp"
#include <cassert>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdint>
#include <stdexcept>
#include <vector>
using oreslang::runtime::FrozenTcpSlab;

int main() {
    std::vector<std::uint8_t> source{0,1,127,128,255};
    FrozenTcpSlab slab(source);
    source.assign(source.size(),0);
    auto first=slab.try_pin();
    auto second=slab.try_pin();
    assert(first && second && slab.size()==5);
    assert(slab.read_byte(*first,3).value()==128);
    assert(slab.read_byte(*first,4).value()==255);
    assert(!slab.read_byte(*first,5));
    FrozenTcpSlab foreign(std::vector<std::uint8_t>{42});
    assert(!foreign.read_byte(*first,0));
    assert(!foreign.release_pin(*first));
    slab.retire();
    assert(!slab.try_pin());
    assert(!slab.freed());
    assert(slab.release_pin(*first));
    assert(!slab.release_pin(*first));
    assert(!slab.read_byte(*first,3));
    assert(slab.read_byte(*second,3).value()==128);
    assert(slab.release_pin(*second));
    assert(slab.freed() && slab.pin_count()==0);
    assert(slab.size()==5); // Metadata survives freed native backing.
    bool rejected=false;
    try {
        FrozenTcpSlab overflow(std::vector<std::uint8_t>(FrozenTcpSlab::max_bytes+1,0));
    } catch (const std::length_error&) { rejected=true; }
    assert(rejected);
    FrozenTcpSlab empty(std::vector<std::uint8_t>{});
    auto empty_pin=empty.try_pin();
    assert(empty_pin && !empty.read_byte(*empty_pin,0));
    empty.retire();
    assert(empty.release_pin(*empty_pin) && empty.freed());
    assert(empty.size()==0 && !empty.try_pin());
    // RAII release and move preserve exactly one lifetime pin.
    FrozenTcpSlab moved(std::vector<std::uint8_t>{17});
    {
        auto original=moved.try_pin();
        assert(original);
        auto transferred=std::move(*original);
        assert(moved.read_byte(transferred,0).value()==17);
        moved.retire();
    }
    assert(moved.freed());
    assert(moved.size()==1);
    FrozenTcpSlab immediate(std::vector<std::uint8_t>{9,10});
    immediate.retire();
    immediate.retire();
    assert(immediate.freed() && immediate.size()==2);
    assert(!immediate.try_pin());

    // Move-only pin keeps the snapshot alive after its host owner is dropped.
    std::optional<FrozenTcpSlab::Pin> surviving;
    {
        auto temporary = std::make_unique<FrozenTcpSlab>(
            std::vector<std::uint8_t>{42});
        auto token = temporary->try_pin();
        assert(token);
        surviving.emplace(std::move(*token));
    }
    surviving.reset(); // ASan checks that this is not use-after-free.

    // Eight host threads hold independent pins across retirement. No thread
    // may access freed bytes, and retirement must wait for all pins to unwind.
    FrozenTcpSlab concurrent(std::vector<std::uint8_t>{0,1,127,128,255});
    std::atomic<int> ready{0};
    std::atomic<int> failures{0};
    std::atomic<bool> proceed{false};
    std::array<std::thread, 8> readers;
    for (auto& worker : readers) {
        worker = std::thread([&]() {
            auto pinned = concurrent.try_pin();
            if (!pinned) {
                failures.fetch_add(1, std::memory_order_relaxed);
                ready.fetch_add(1, std::memory_order_release);
                return;
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!proceed.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 1024; ++i) {
                if (concurrent.read_byte(*pinned,3) != 128)
                    failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != 8)
        std::this_thread::yield();
    concurrent.retire();
    assert(!concurrent.try_pin());
    assert(!concurrent.freed());
    proceed.store(true, std::memory_order_release);
    for (auto& worker : readers) worker.join();
    assert(concurrent.freed() && concurrent.pin_count() == 0);
    assert(failures.load(std::memory_order_relaxed) == 0);

    // Wrong-owner read takes the *other* slab's lock. Prior code loaded a
    // non-atomic pin.active_ while its owning thread released that pin,
    // creating a C++ data race. Keep the pin object alive until readers join.
    FrozenTcpSlab owned(std::vector<std::uint8_t>{17});
    FrozenTcpSlab unrelated(std::vector<std::uint8_t>{19});
    auto receipt = owned.try_pin();
    assert(receipt);
    std::atomic<bool> read_ready{false};
    std::atomic<bool> read_finished{false};
    std::thread foreign_reader([&] {
        read_ready.store(true, std::memory_order_release);
        for (int i = 0; i < 65536; ++i) {
            if (unrelated.read_byte(*receipt, 0).has_value())
                failures.fetch_add(1, std::memory_order_relaxed);
        }
        read_finished.store(true, std::memory_order_release);
    });
    while (!read_ready.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(owned.release_pin(*receipt));
    foreign_reader.join();
    assert(read_finished.load(std::memory_order_acquire));
    assert(!owned.release_pin(*receipt));
    owned.retire();
    assert(owned.freed() && failures.load(std::memory_order_relaxed) == 0);
    return 0;
}
