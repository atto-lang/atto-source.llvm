#include "oreslang/runtime/ActorMailboxCompletion.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorMailboxCompletion;
using Status = ActorMailboxCompletion::DrainStatus;
#define CHECK(ok) do { if (!(ok)) throw std::runtime_error("mailbox completion model check failed: " #ok); } while (false)

static void randomized_reference_model() {
    constexpr std::size_t WINDOW = 7;
    ActorMailboxCompletion subject(WINDOW);
    std::mt19937_64 rng(0x70A5B0C1D2E3ULL);
    std::map<std::uint64_t, int> receipts; // 0=pending, 1=processed, 2=aborted
    std::size_t preparing = 0;
    std::uint64_t next = 1, watermark = 0, first_aborted = 0;
    for (int step = 0; step < 300000; ++step) {
        const auto operation = rng() % 7;
        const bool may_reserve = (next - 1 - watermark + preparing) < WINDOW;
        if (operation <= 2) {
            CHECK(subject.reserve_prepare() == may_reserve);
            if (may_reserve) ++preparing;
        } else if (operation == 3 && preparing) {
            subject.cancel_prepare();
            --preparing;
        } else if (operation == 4 && preparing) {
            if ((rng() % 5) == 0) {
                bool thrown = false;
                try { (void)subject.publish([](std::uint64_t) {
                    throw std::runtime_error("fault injected before enqueue");
                }); } catch (const std::runtime_error&) { thrown = true; }
                CHECK(thrown);
                CHECK(subject.snapshot() == next - 1);
            } else {
                const auto sequence = subject.publish([&](std::uint64_t id) {
                    CHECK(id == next);
                });
                CHECK(sequence == next);
                receipts.emplace(next++, 0);
                --preparing;
            }
        } else if (operation >= 5 && !receipts.empty()) {
            const auto index = rng() % receipts.size();
            auto item = receipts.begin();
            std::advance(item, static_cast<long>(index));
            if (item->second == 0) {
                const bool abort = (rng() & 7) == 0;
                subject.complete(item->first, abort);
                item->second = abort ? 2 : 1;
                if (abort && (!first_aborted || item->first < first_aborted))
                    first_aborted = item->first;
                bool duplicate_rejected = false;
                try { subject.complete(item->first, !abort); }
                catch (const std::logic_error&) { duplicate_rejected = true; }
                CHECK(duplicate_rejected);
            }
        }
        while (!receipts.empty() && receipts.begin()->first == watermark + 1
               && receipts.begin()->second != 0) {
            watermark = receipts.begin()->first;
            receipts.erase(receipts.begin());
        }
        CHECK(subject.snapshot() == next - 1);
        CHECK(subject.watermark() == watermark);
        CHECK(next - 1 - watermark + preparing <= WINDOW);
        if (next > 1) {
            const std::uint64_t target = rng() % next;
            const auto expected = watermark < target ? Status::Pending
                : first_aborted && first_aborted <= target ? Status::Aborted
                : Status::Delivered;
            CHECK(subject.drain_status(target) == expected);
            CHECK(subject.completed_through(target) == (expected == Status::Delivered));
        }
    }
}

static void concurrent_publish_and_out_of_order_terminal_ack() {
    constexpr int PRODUCERS = 4, CONSUMERS = 4, EACH = 12500;
    constexpr std::uint64_t TOTAL = PRODUCERS * EACH;
    ActorMailboxCompletion subject(128);
    std::mutex pending_mutex;
    std::deque<std::uint64_t> pending;
    std::atomic<int> produced{0}, consumed{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < EACH; ++i) {
                while (!subject.reserve_prepare()) std::this_thread::yield();
                auto seq = subject.publish([&](std::uint64_t seq) {
                    std::lock_guard<std::mutex> g(pending_mutex);
                    pending.push_back(seq);
                });
                CHECK(seq <= TOTAL);
                ++produced;
            }
        });
    }
    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&, c] {
            for (;;) {
                std::uint64_t seq = 0;
                {
                    std::lock_guard<std::mutex> g(pending_mutex);
                    if (!pending.empty()) {
                        if (c % 2) {
                            seq = pending.back(); pending.pop_back();
                        } else {
                            seq = pending.front(); pending.pop_front();
                        }
                    }
                }
                if (seq) {
                    subject.complete(seq, seq % 997 == 0);
                    ++consumed;
                } else if (consumed.load() == static_cast<int>(TOTAL)
                           && produced.load() == static_cast<int>(TOTAL)) break;
                else std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(produced == TOTAL && consumed == TOTAL);
    CHECK(subject.snapshot() == TOTAL && subject.watermark() == TOTAL);
    CHECK(subject.drain_status(TOTAL) == Status::Aborted);
    CHECK(subject.drain_status(1) == Status::Delivered);
    CHECK(subject.reserve_prepare());
    subject.cancel_prepare();
}

int main() {
    randomized_reference_model();
    concurrent_publish_and_out_of_order_terminal_ack();
}
