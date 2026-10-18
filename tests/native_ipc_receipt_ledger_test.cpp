#include "oreslang/runtime/NativeIpcReceiptLedger.hpp"
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>

using namespace oreslang::runtime;

void check(bool ok) {
    if (!ok) throw std::logic_error("native IPC test check failed");
}

template<typename F> void rejected(F fn) {
    bool caught = false;
    try { fn(); } catch (const std::exception&) { caught = true; }
    check(caught);
}

static IpcReceiptIdentity id(std::uint64_t seq = 1) {
    return {"tenant", "sender", 4, "worker", 9, 2, "session",
            seq, 1, 3, std::string(64, 'a')};
}

int main() {
    NativeIpcReceiptLedger ledger(2);
    auto first = id(), second = id(2);
    check(ledger.admit(first) == IpcReceiptStage::local_admitted);
    check(ledger.invariant() && ledger.transport_credits() == 1);
    rejected([&] { ledger.admit(first); });
    auto forged = first; forged.transport_session = "other";
    rejected([&] { ledger.transition(forged, IpcReceiptStage::remote_accepted, true); });
    rejected([&] { ledger.transition(first, IpcReceiptStage::handler_committed, true); });
    rejected([&] { ledger.transition(first, IpcReceiptStage::remote_accepted, false); });
    ledger.transition(first, IpcReceiptStage::remote_accepted, true);
    check(ledger.transport_credits() == 0 && ledger.live_count() == 1);
    rejected([&] { ledger.transition(first, IpcReceiptStage::rejected_before_remote, true); });
    rejected([&] { ledger.transition(first, IpcReceiptStage::remote_accepted, true); });
    ledger.transition(first, IpcReceiptStage::handler_failed, true);
    check(ledger.invariant() && ledger.live_count() == 0);
    rejected([&] { ledger.transition(first, IpcReceiptStage::handler_committed, true); });
    check(!ledger.can_replay(first));
    ledger.admit(second);
    auto third = id(3); third.route_epoch = 3;
    ledger.admit(third);
    rejected([&] { ledger.admit(id(4)); });
    ledger.transition(second, IpcReceiptStage::unknown, true);
    rejected([&] { ledger.transition(third, IpcReceiptStage::handler_committed, false); });
    ledger.transition(third, IpcReceiptStage::remote_accepted, true);
    ledger.transition(third, IpcReceiptStage::handler_committed, true);
    check(ledger.invariant() && ledger.live_count() == 0);
    auto stale = id(4); stale.route_epoch = 1;
    rejected([&] { ledger.admit(stale); });
    auto stale_cap = id(4); stale_cap.capability_generation = 2;
    rejected([&] { ledger.admit(stale_cap); });
    auto valid = id(4); valid.route_epoch = 3;
    ledger.admit(valid);
    ledger.transition(valid, IpcReceiptStage::rejected_before_remote, true);
    check(ledger.invariant() && ledger.live_count() == 0);
    auto too_large = id(5); too_large.message_sequence = UINT64_MAX;
    rejected([&] { ledger.admit(too_large); });
    check(ledger.invariant() && ledger.live_count() == 0);
    // Reproducible interleavings of admissions, receipt delivery, terminal
    // cancellation and mismatched-session attacks; no transport is involved.
    for (std::uint64_t scenario = 1; scenario <= 1024; ++scenario) {
        const std::size_t capacity = 1 + static_cast<std::size_t>(scenario % 5);
        NativeIpcReceiptLedger book(capacity);
        std::vector<std::pair<IpcReceiptIdentity, bool>> pending;
        std::uint64_t sequence = 1;
        std::uint64_t random = 0x9e3779b97f4a7c15ULL ^ scenario;
        auto next = [&random]() { random ^= random << 13; random ^= random >> 7;
            random ^= random << 17; return random; };
        for (int step = 0; step < 64; ++step) {
            const auto op = next() % 6;
            if (pending.empty() || op == 0) {
                auto message = id(sequence); message.route_epoch = 3;
                ++sequence;
                if (pending.size() >= capacity)
                    rejected([&] { book.admit(message); });
                else {
                    check(book.admit(message) == IpcReceiptStage::local_admitted);
                    pending.emplace_back(std::move(message), false);
                }
            } else {
                const auto index = static_cast<std::size_t>(next() % pending.size());
                const auto [message, received] = pending[index];
                if (op == 1 && !received) {
                    book.transition(message, IpcReceiptStage::remote_accepted, true);
                    pending[index].second = true;
                } else if (op == 2) {
                    auto forged_message = message;
                    forged_message.transport_session = "forged";
                    rejected([&] { book.transition(forged_message,
                        IpcReceiptStage::remote_accepted, true); });
                } else if (op == 3 && received) {
                    rejected([&] { book.transition(message,
                        IpcReceiptStage::rejected_before_remote, true); });
                } else {
                    const auto terminal = received
                        ? ((next() & 1) ? IpcReceiptStage::handler_committed
                                        : IpcReceiptStage::handler_failed)
                        : IpcReceiptStage::unknown;
                    book.transition(message, terminal, true);
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                    rejected([&] { book.transition(message, terminal, true); });
                }
            }
            check(book.invariant());
            check(book.live_count() == pending.size());
            std::size_t expected_credits = 0;
            for (const auto& entry : pending) if (!entry.second) ++expected_credits;
            check(book.transport_credits() == expected_credits);
        }
        for (const auto& entry : pending)
            book.transition(entry.first, IpcReceiptStage::unknown, true);
        check(book.live_count() == 0 && book.transport_credits() == 0);
        check(book.invariant());
    }
    // Multiple transport completion lanes race on independent receipts. The
    // ledger must serialize all correlations without losing any terminal ACK.
    {
        constexpr std::size_t messages = 512;
        NativeIpcReceiptLedger concurrent(messages);
        for (std::uint64_t seq = 1; seq <= messages; ++seq)
            concurrent.admit(id(seq));
        std::atomic<int> errors{0};
        std::vector<std::thread> workers;
        for (std::uint64_t lane = 0; lane < 8; ++lane) {
            workers.emplace_back([&concurrent, &errors, lane]() {
                for (std::uint64_t seq = lane + 1; seq <= messages; seq += 8) {
                    try {
                        auto msg = id(seq);
                        concurrent.transition(msg, IpcReceiptStage::remote_accepted, true);
                        concurrent.transition(msg, IpcReceiptStage::handler_committed, true);
                    } catch (const std::exception&) { ++errors; }
                }
            });
        }
        for (auto& thread : workers) thread.join();
        check(errors.load() == 0);
        check(concurrent.invariant());
        check(concurrent.live_count() == 0);
        check(concurrent.transport_credits() == 0);
    }
    return 0;
}
