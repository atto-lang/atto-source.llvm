#include "oreslang/runtime/NativeIpcReceiptLedger.hpp"
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

static IpcReceiptEnvelope env(std::uint64_t sequence = 1) {
    return {"tenant", "caller", 4, "worker", 7, 3, "transport", sequence, 1, 2,
            std::string(64, 'a')};
}
static void check(bool condition) { if (!condition) throw std::runtime_error("IPC invariant"); }

template <typename F> static void deny(F&& call) {
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
int main() {
    NativeIpcReceiptLedger ledger(2);
    auto e = env();
    ledger.admit(e);
    check(ledger.budgets().first == 1 && ledger.budgets().second == 1);
    deny([&] { ledger.advance(e, IpcReceiptStage::HandlerCommitted, true); });
    deny([&] { ledger.advance(e, IpcReceiptStage::RemoteAccepted, false); });
    auto forged = e; forged.capability_generation = 9;
    deny([&] { ledger.advance(forged, IpcReceiptStage::RemoteAccepted, true); });
    ledger.advance(e, IpcReceiptStage::RemoteAccepted, true);
    check(ledger.budgets().first == 1 && ledger.budgets().second == 0);
    deny([&] { ledger.advance(e, IpcReceiptStage::RejectedBeforeAdmission, true); });
    ledger.advance(e, IpcReceiptStage::HandlerCommitted, true);
    deny([&] { ledger.advance(e, IpcReceiptStage::HandlerCommitted, true); });
    deny([&] { ledger.admit(e); });
    check(ledger.invariant());

    auto two = env(2); two.route_epoch = 4; two.capability_generation = 3;
    ledger.admit(two);
    auto stale = env(3);
    deny([&] { ledger.admit(stale); });
    stale.route_epoch = 4; stale.capability_generation = 3;
    ledger.admit(stale);
    deny([&] { ledger.admit(env(4)); }); // full and/or rollback
    check(ledger.budgets().first == 2 && ledger.budgets().second == 2);
    ledger.advance(two, IpcReceiptStage::RejectedBeforeAdmission, true);
    ledger.advance(stale, IpcReceiptStage::RemoteAccepted, true);
    ledger.advance(stale, IpcReceiptStage::HandlerFailed, true);
    check(ledger.budgets().first == 0 && ledger.invariant());
    check(!NativeIpcReceiptLedger::can_replay_automatically());

    NativeIpcReceiptLedger crash(1);
    crash.admit(env());
    check(crash.mark_transport_lost() == 1);
    check(crash.mark_transport_lost() == 0);
    deny([&] { crash.advance(env(), IpcReceiptStage::HandlerCommitted, true); });
    deny([&] { crash.admit(env()); });
    check(crash.invariant());

    for (auto mutation : {1, 2, 3, 4, 5}) {
        auto wrong = env();
        switch (mutation) {
        case 1: wrong.sender_domain = "other!"; break;
        case 2: wrong.sender_incarnation = 0; break;
        case 3: wrong.payload_sha256[1] = 'G'; break;
        case 4: wrong.transport_session = "bad/session"; break;
        default: wrong.message_sequence = std::uint64_t{1} << 63; break;
        }
        deny([&] { NativeIpcReceiptLedger x(1); x.admit(wrong); });
    }
    deny([] { NativeIpcReceiptLedger x(0); });
    deny([] { NativeIpcReceiptLedger x(65537); });

    // Race: same message can be admitted only once; no capacity bypass.
    NativeIpcReceiptLedger concurrent(8);
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) {
        workers.emplace_back([&, i] {
            for (int n = 0; n < 1000; ++n) {
                try {
                    auto v = env(static_cast<std::uint64_t>(n + 1 + i * 1000));
                    concurrent.admit(v);
                    concurrent.advance(v, IpcReceiptStage::RemoteAccepted, true);
                    concurrent.advance(v, IpcReceiptStage::HandlerCommitted, true);
                } catch (const std::logic_error&) { /* sequence/race rejection */ }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    check(concurrent.invariant());
    check(concurrent.budgets().first <= 8);
    (void)concurrent.mark_transport_lost();
    check(concurrent.budgets().first == 0);
}
