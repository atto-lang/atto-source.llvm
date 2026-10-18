#include "oreslang/runtime/NativeIpcReceiptLedger.hpp"

#include <cassert>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

static IpcReceiptKey envelope(std::uint64_t seq = 1) {
    return {"tenantA", "caller", "worker", "sessionA",
            std::string(64, 'a'), 4, 7, 8, seq, 1, 2};
}

template <typename F> static void reject(F&& function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    assert(failed && "expected strict rejection");
}

int main() {
    NativeIpcReceiptLedger ledger(2);
    auto first = envelope();
    ledger.admit(first);
    assert(ledger.budgets() == std::make_pair(std::size_t{1}, std::size_t{1}));
    reject([&] { ledger.advance_verified(first, IpcReceiptStage::HandlerCommitted); });
    auto forged = first;
    forged.sha256[1] = 'b';
    reject([&] { ledger.advance_verified(forged, IpcReceiptStage::RemoteAccepted); });
    ledger.advance_verified(first, IpcReceiptStage::RemoteAccepted);
    assert(ledger.budgets() == std::make_pair(std::size_t{1}, std::size_t{0}));
    reject([&] { ledger.advance_verified(first, IpcReceiptStage::RejectedBeforeRemote); });
    ledger.advance_verified(first, IpcReceiptStage::HandlerCommitted);
    reject([&] { ledger.advance_verified(first, IpcReceiptStage::HandlerCommitted); });
    reject([&] { ledger.admit(first); });
    assert(ledger.invariant());

    auto second = envelope(2);
    second.route_epoch = 9;
    second.capability_generation = 3;
    ledger.admit(second);
    auto stale = envelope(3);
    reject([&] { ledger.admit(stale); });
    stale.route_epoch = 9;
    stale.capability_generation = 3;
    ledger.admit(stale);
    reject([&] { ledger.admit(envelope(4)); });
    assert(ledger.budgets() == std::make_pair(std::size_t{2}, std::size_t{2}));
    ledger.advance_verified(second, IpcReceiptStage::RejectedBeforeRemote);
    ledger.advance_verified(stale, IpcReceiptStage::RemoteAccepted);
    ledger.advance_verified(stale, IpcReceiptStage::HandlerFailed);
    assert(ledger.invariant());
    assert(ledger.budgets().first == 0);

    auto next = envelope(4);
    next.route_epoch = 10;
    next.capability_generation = 4;
    ledger.admit(next);
    auto mismatch = next;
    mismatch.destination_incarnation = 8;
    reject([&] { ledger.advance_verified(mismatch, IpcReceiptStage::RemoteAccepted); });
    assert(ledger.mark_transport_lost() == 1);
    assert(ledger.mark_transport_lost() == 0);
    reject([&] { ledger.admit(next); });
    reject([&] { ledger.advance_verified(next, IpcReceiptStage::HandlerCommitted); });
    assert(!NativeIpcReceiptLedger::may_replay_automatically());

    for (int mutation = 0; mutation < 6; ++mutation) {
        auto wrong = envelope();
        switch (mutation) {
            case 0: wrong.domain = "other!"; break;
            case 1: wrong.sender_incarnation = 0; break;
            case 2: wrong.sha256[0] = 'G'; break;
            case 3: wrong.session = "../traversal"; break;
            case 4: wrong.message_sequence = std::uint64_t{1} << 63; break;
            default: wrong.sha256 = "short"; break;
        }
        reject([&] { NativeIpcReceiptLedger test(1); test.admit(wrong); });
    }
    reject([] { NativeIpcReceiptLedger test(0); });
    reject([] { NativeIpcReceiptLedger test(65537); });

    NativeIpcReceiptLedger concurrent(8);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&, i] {
            for (int j = 0; j < 2000; ++j) {
                auto msg = envelope(static_cast<std::uint64_t>(i * 2000 + j + 1));
                try {
                    concurrent.admit(msg);
                    concurrent.advance_verified(msg, IpcReceiptStage::RemoteAccepted);
                    concurrent.advance_verified(msg, IpcReceiptStage::HandlerCommitted);
                } catch (const std::logic_error&) {
                    // Expected race losses: no sequence replay, no capacity bypass.
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(concurrent.invariant());
    assert(concurrent.budgets().first <= 8);
    concurrent.mark_transport_lost();
    assert(concurrent.budgets() == std::make_pair(std::size_t{0}, std::size_t{0}));
    return 0;
}
