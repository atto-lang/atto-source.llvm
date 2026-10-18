#include "oreslang/runtime/NativeIpcReceiptLedger.hpp"
#include <stdexcept>
#include <string>
#include <utility>

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
    return 0;
}
