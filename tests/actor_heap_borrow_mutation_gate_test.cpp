#include "oreslang/runtime/ActorHeapRetirementProof.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::ActorHeapBorrowGate;
using oreslang::runtime::ActorHeapIdentity;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void deterministic_exclusion_and_retirement() {
    const ActorHeapIdentity owner{100, 200, 300};
    ActorHeapBorrowGate gate(owner);
    auto a = gate.acquire(owner);
    auto b = gate.acquire(owner);
    check(a.has_value() && b.has_value(), "simultaneous shared reads were rejected");
    check(!gate.try_mutation(owner).has_value(), "mutation admitted with shared loans");
    a->reset();
    check(!gate.try_mutation(owner).has_value(), "second live loan was ignored");
    b->reset();

    auto writer = gate.try_mutation(owner);
    check(writer.has_value(), "writer unavailable after all shared loans ended");
    check(!gate.acquire(owner).has_value(), "FFI lease admitted while writer active");
    check(!gate.try_mutation(owner).has_value(), "second writer admitted");

    // Retirement must account for the exclusive writer, not just readers.
    check(!gate.seal_and_quiesced(owner), "retired under in-flight mutation");
    check(!gate.acquire(owner).has_value(), "read admitted after retirement seal");
    check(!gate.try_mutation(owner).has_value(), "writer admitted after retirement seal");
    check(!gate.quiesced(), "sealed but still-mutating gate reported quiesced");
    writer->reset();
    check(gate.quiesced(), "writer completion failed to quiesce sealed gate");
    check(gate.seal_and_quiesced(owner), "repeated retirement seal not idempotent");

    const ActorHeapIdentity foreign{100, 201, 300};
    bool denied = false;
    try { (void)gate.try_mutation(foreign); }
    catch (const std::logic_error&) { denied = true; }
    check(denied, "foreign actor acquired mutation authority");
}

static void competing_reader_and_writer_race() {
    const ActorHeapIdentity owner{9, 10, 11};
    ActorHeapBorrowGate gate(owner);
    std::atomic<int> readers{0};
    std::atomic<bool> writer{false};
    std::atomic<bool> violation{false};
    std::atomic<int> read_wins{0};
    std::atomic<int> write_wins{0};

    std::thread read_thread([&] {
        for (int i = 0; i < 30000; ++i) {
            auto pin = gate.acquire(owner);
            if (!pin) { std::this_thread::yield(); continue; }
            readers.fetch_add(1, std::memory_order_seq_cst);
            if (writer.load(std::memory_order_seq_cst))
                violation.store(true, std::memory_order_relaxed);
            read_wins.fetch_add(1, std::memory_order_relaxed);
            readers.fetch_sub(1, std::memory_order_seq_cst);
        }
    });
    std::thread write_thread([&] {
        for (int i = 0; i < 30000; ++i) {
            auto pin = gate.try_mutation(owner);
            if (!pin) { std::this_thread::yield(); continue; }
            writer.store(true, std::memory_order_seq_cst);
            if (readers.load(std::memory_order_seq_cst) != 0)
                violation.store(true, std::memory_order_relaxed);
            write_wins.fetch_add(1, std::memory_order_relaxed);
            writer.store(false, std::memory_order_seq_cst);
        }
    });
    read_thread.join();
    write_thread.join();
    check(read_wins.load() > 0, "race never admitted any shared borrower");
    check(write_wins.load() > 0, "race never admitted an exclusive mutation");
    check(!violation.load(), "shared loan and mutation overlapped");
    check(!gate.busy(), "racing borrow gate retained stale admission");
}

int main() {
    deterministic_exclusion_and_retirement();
    competing_reader_and_writer_race();
    std::cout << "PASS actor pointer loan/mutation atomic exclusion\\n";
}
