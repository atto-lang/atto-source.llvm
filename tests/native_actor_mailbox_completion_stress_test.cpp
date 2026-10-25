// Exact production header regression: GCC/Clang + ASan/UBSan checked at
// ActorMailboxCompletion.hpp blob 4852904241455d4e800c3c43657923fbda3ad54a.
// This does not claim source-level .ores compiler or scheduler parity.
#include "oreslang/runtime/ActorMailboxCompletion.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorMailboxCompletion;
using Status = ActorMailboxCompletion::DrainStatus;

template<class Fn>
void expect_throw(const char* name, Fn fn) {
    bool thrown = false;
    try { fn(); } catch (const std::exception&) { thrown = true; }
    if (!thrown) {
        std::cerr << "missing exception: " << name << "\n";
        std::abort();
    }
}

int main() {
    int passed = 0;
    auto ok = [&](bool condition, const char* label) {
        if (!condition) {
            std::cerr << "FAIL " << label << "\n";
            std::abort();
        }
        std::cout << "PASS " << label << "\n";
        ++passed;
    };

    expect_throw("zero window", [] { ActorMailboxCompletion zero(0); });
    ok(true, "zero-window rejected");

    {
        ActorMailboxCompletion m(2);
        expect_throw("uncommitted drain", [&] { (void)m.drain_status(1); });
        expect_throw("publish missing reservation", [&] { (void)m.publish([](auto) {}); });
        ok(m.snapshot() == 0 && m.watermark() == 0, "empty ledger and admission checks");

        ok(m.reserve_prepare() && m.reserve_prepare() && !m.reserve_prepare(),
           "bounded prepare capacity");
        const auto a = m.publish([](auto seq) { if (seq != 1) throw std::logic_error("unexpected first committed sequence"); });
        const auto b = m.publish([](auto seq) { if (seq != 2) throw std::logic_error("unexpected second committed sequence"); });
        ok(a == 1 && b == 2 && !m.reserve_prepare(),
           "monotonic publication and window backpressure");

        m.complete(b);
        ok(m.watermark() == 0 && m.drain_status(b) == Status::Pending,
           "out-of-order ACK cannot overtake earlier frame");
        m.complete(a);
        ok(m.watermark() == 2 && m.drain_status(b) == Status::Delivered,
           "contiguous fence advances only across complete prefix");
        expect_throw("duplicate completion", [&] { m.complete(a); });
        ok(m.reserve_prepare(), "ACK restores bounded capacity");
        m.cancel_prepare();
        ok(m.snapshot() == 2, "cancelled preparation does not consume sequence");
    }

    {
        ActorMailboxCompletion m(1);
        ok(m.reserve_prepare(), "exception test reserves slot");
        expect_throw("throwing publish", [&] {
            m.publish([](auto) { throw std::runtime_error("synthetic allocation failure"); });
        });
        ok(m.snapshot() == 0 && !m.reserve_prepare(),
           "failed publication preserves sequence and reservation");
        m.cancel_prepare();
        ok(m.reserve_prepare() && m.publish([](auto seq) { if (seq != 1) throw std::logic_error("unexpected first committed sequence"); }) == 1,
           "cancel then retry preserves sequence identity");
        m.complete(1, true);
        ok(m.drain_status(1) == Status::Aborted && !m.completed_through(1),
           "aborted retirement poisons drain");
        ok(m.reserve_prepare() && m.publish([](auto seq) { if (seq != 2) throw std::logic_error("unexpected second committed sequence"); }) == 2,
           "subsequent publisher does not erase aborted fence");
        m.complete(2);
        ok(m.drain_status(2) == Status::Aborted,
           "later success does not disguise earlier aborted delivery");
    }

    {
        ActorMailboxCompletion m(32);
        constexpr int workers = 4;
        constexpr int iters = 2000;
        std::atomic<int> exceptions{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < workers; ++i) {
            threads.emplace_back([&] {
                try {
                    for (int j = 0; j < iters; ++j) {
                        while (!m.reserve_prepare()) std::this_thread::yield();
                        const auto seq = m.publish([](auto) {});
                        if (seq % 9 == 0) std::this_thread::yield();
                        m.complete(seq);
                    }
                } catch (...) { ++exceptions; }
            });
        }
        for (auto& t : threads) t.join();

        ok(exceptions == 0 && m.snapshot() == workers * iters &&
               m.watermark() == workers * iters &&
               m.drain_status(workers * iters) == Status::Delivered,
           "4-thread 8000-message adversarial publication and completion");
    }

    std::cout << "TOTAL_PASS " << passed << '\n';
}
