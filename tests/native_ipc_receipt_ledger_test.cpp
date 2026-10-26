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
    // Reconciled transport-loss and admission attacks from the older API.
    {
        NativeIpcReceiptLedger lost(2);
        auto local = id(1), accepted = id(2);
        lost.admit(local); lost.admit(accepted);
        lost.transition(accepted, IpcReceiptStage::remote_accepted, true);
        check(lost.transport_credits() == 1);
        check(lost.mark_transport_lost() == 2);
        check(lost.mark_transport_lost() == 0);
        check(lost.invariant() && lost.live_count() == 0 && lost.transport_credits() == 0);
        rejected([&] { lost.admit(local); });
        rejected([&] { lost.transition(accepted, IpcReceiptStage::handler_committed, true); });
        check(!lost.can_replay(local) && !lost.can_replay(accepted));
        lost.admit(id(3));
        check(lost.mark_transport_lost() == 1);
    }
    // Loss must retain every watermark independently, even for a fresh
    // sequence/session, and a rejected admission must not consume sequence.
    {
        NativeIpcReceiptLedger lost(2);
        auto prior = id(40); prior.route_epoch = 9; prior.capability_generation = 12;
        lost.admit(prior);
        check(lost.mark_transport_lost() == 1);
        auto next = id(41); next.route_epoch = 9; next.capability_generation = 12;
        next.transport_session = "replacement";
        auto stale_route = next; stale_route.route_epoch = 8;
        rejected([&] { lost.admit(stale_route); });
        auto stale_capability = next; stale_capability.capability_generation = 11;
        rejected([&] { lost.admit(stale_capability); });
        auto foreign_stream = next; ++foreign_stream.sender_incarnation;
        rejected([&] { lost.admit(foreign_stream); });
        check(lost.invariant() && lost.live_count() == 0 && lost.transport_credits() == 0);
        lost.admit(next);
        lost.transition(next, IpcReceiptStage::rejected_before_remote, true);
        check(lost.invariant() && lost.live_count() == 0 && lost.transport_credits() == 0);
    }
    // Completion and loss must partition the admitted population exactly:
    // each message either commits first or is removed once as UNKNOWN.
    for (unsigned round = 0; round < 128; ++round) {
        constexpr std::uint64_t messages = 128;
        NativeIpcReceiptLedger race(messages);
        for (std::uint64_t seq = 1; seq <= messages; ++seq) race.admit(id(seq));
        std::atomic<bool> start{false};
        std::atomic<unsigned> committed{0}, unexpected{0};
        std::thread completion([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t seq = 1; seq <= messages; ++seq) {
                try {
                    race.transition(id(seq), IpcReceiptStage::remote_accepted, true);
                    std::this_thread::yield();
                    race.transition(id(seq), IpcReceiptStage::handler_committed, true);
                    ++committed;
                } catch (const std::invalid_argument&) {
                    // Loss linearized before this receipt.
                } catch (...) { ++unexpected; }
            }
        });
        start.store(true, std::memory_order_release);
        if (round % 2) std::this_thread::yield();
        const auto unknown = race.mark_transport_lost();
        completion.join();
        check(unexpected.load() == 0 && unknown + committed.load() == messages);
        check(race.mark_transport_lost() == 0 && race.invariant());
        check(race.live_count() == 0 && race.transport_credits() == 0);
        rejected([&] { race.admit(id(messages)); });
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto invalid = id();
        switch (mutation) {
            case 0: invalid.tenant = "other!"; break;
            case 1: invalid.sender_incarnation = 0; break;
            case 2: invalid.payload_sha256[0] = 'G'; break;
            case 3: invalid.transport_session = "../traversal"; break;
            case 4: invalid.message_sequence = std::uint64_t{1} << 63; break;
            default: invalid.payload_sha256 = "short"; break;
        }
        rejected([&] { NativeIpcReceiptLedger probe(1); probe.admit(invalid); });
    }
    rejected([] { NativeIpcReceiptLedger probe(0); });
    rejected([] { NativeIpcReceiptLedger probe(65537); });
    {
        NativeIpcReceiptLedger attempts(8);
        std::atomic<int> unexpected{0};
        std::vector<std::thread> workers;
        for (std::uint64_t lane = 0; lane < 8; ++lane) {
            workers.emplace_back([&, lane] {
                for (std::uint64_t j = 0; j < 2000; ++j) {
                    auto msg = id(lane * 2000 + j + 1);
                    try {
                        attempts.admit(msg);
                        attempts.transition(msg, IpcReceiptStage::remote_accepted, true);
                        attempts.transition(msg, IpcReceiptStage::handler_committed, true);
                    } catch (const std::invalid_argument&) {
                        // Admission raced a later monotonic sequence.
                    } catch (const std::overflow_error&) {
                        // Bounded correlation admission can be full.
                    } catch (...) { ++unexpected; }
                }
            });
        }
        for (auto& worker : workers) worker.join();
        check(unexpected.load() == 0);
        check(attempts.invariant() && attempts.live_count() <= 8);
        attempts.mark_transport_lost();
        check(attempts.invariant() && attempts.live_count() == 0 && attempts.transport_credits() == 0);
    }
    return 0;
}

