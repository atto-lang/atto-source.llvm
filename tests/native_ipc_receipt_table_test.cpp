#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace oreslang::runtime;

static IpcReceiptEnvelope envelope(std::uint64_t seq = 1) {
    IpcReceiptEnvelope e;
    e.domain="tenant"; e.sender="sender"; e.destination="receiver";
    e.session="session"; e.payload_sha256=std::string(64,'a');
    e.sender_incarnation=2; e.destination_incarnation=3;
    e.route_epoch=4; e.capability_generation=5;
    e.message_sequence=seq; e.frame_sequence=seq;
    return e;
}
template<class F> static void rejects(F f) {
    try { f(); } catch(const std::invalid_argument&) { return; }
    catch(const std::overflow_error&) { return; }
    assert(false && "admission/receipt unexpectedly accepted");
}
int main() {
    rejects([] { NativeIpcReceiptTable table(0); });
    // Java protocol-v3 identities are positive signed long values. Native
    // uint64_t storage must not admit values Java cannot represent.
    {
        NativeIpcReceiptTable numeric_domain(1);
        auto too_large = envelope();
        too_large.message_sequence =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        rejects([&]{ numeric_domain.admit(too_large); });
        too_large = envelope();
        too_large.frame_sequence =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        rejects([&]{ numeric_domain.admit(too_large); });
        too_large = envelope();
        too_large.route_epoch =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        rejects([&]{ numeric_domain.admit(too_large); });
    }
    NativeIpcReceiptTable table(2);
    auto first=envelope(1), second=envelope(2), third=envelope(3);
    table.admit(first);
    assert(table.counters() == (std::pair<std::size_t,std::size_t>{1,1}));
    rejects([&]{ table.admit(first); });
    auto forgery=first; forgery.session="other";
    rejects([&]{ table.accept_verified(forgery,IpcReceiptStage::remote_accepted); });
    // Peer-authenticated frames never carry host-local verdict authority.
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::outcome_unknown); });
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::rejected_before_remote); });
    // Remote acceptance is impossible until the local transport commits
    // dispatch before the first possible physical write.
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::remote_accepted); });
    table.commit_dispatch(first);
    rejects([&]{ table.commit_dispatch(first); });
    rejects([&]{ table.reject_before_remote(first); });
    table.accept_verified(first,IpcReceiptStage::remote_accepted);
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::outcome_unknown); });
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::rejected_before_remote); });
    table.admit(second);
    rejects([&]{ table.admit(third); });
    table.accept_verified(first,IpcReceiptStage::handler_failed);
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::handler_committed); });
    table.reject_before_remote(second);
    assert(table.counters() == (std::pair<std::size_t,std::size_t>{0,0}));
    auto rolled=envelope(3); rolled.route_epoch=3;
    rejects([&]{ table.admit(rolled); });
    rolled=envelope(3); rolled.capability_generation=4;
    rejects([&]{ table.admit(rolled); });
    table.admit(third);
    auto oldinc=envelope(4); ++oldinc.destination_incarnation;
    rejects([&]{ table.admit(oldinc); });
    auto frame_rollback=envelope(4); frame_rollback.frame_sequence=3;
    rejects([&]{ table.admit(frame_rollback); });
    table.commit_dispatch(third);
    assert(table.mark_transport_lost(third) == 1);
    assert(table.invariant());
    // Competing authenticated receipts may race on distinct transport carriers;
    // exactly one terminal transition must win without double-credit release.
    NativeIpcReceiptTable racing(1);
    auto race_message = envelope();
    racing.admit(race_message);
    racing.commit_dispatch(race_message);
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int i=0; i<16; ++i) workers.emplace_back([&] {
        try {
            racing.accept_verified(race_message, IpcReceiptStage::remote_accepted);
            ++accepted;
        } catch(const std::invalid_argument&) { }
    });
    for (auto& worker : workers) worker.join();
    assert(accepted.load() == 1);
    assert(racing.counters() == (std::pair<std::size_t,std::size_t>{1,0}));
    workers.clear();
    std::atomic<int> settled{0};
    for (int i=0; i<16; ++i) workers.emplace_back([&] {
        try {
            racing.accept_verified(race_message, IpcReceiptStage::handler_committed);
            ++settled;
        } catch(const std::invalid_argument&) { }
    });
    for (auto& worker : workers) worker.join();
    assert(settled.load() == 1);
    assert(racing.counters() == (std::pair<std::size_t,std::size_t>{0,0}));
    assert(racing.invariant());
    // Competing transport-loss cleanup and remote ACKs must not leak
    // credits or resurrect a previously terminal correlation.
    NativeIpcReceiptTable disconnect_race(1);
    auto in_flight = envelope();
    disconnect_race.admit(in_flight);
    disconnect_race.commit_dispatch(in_flight);
    workers.clear();
    std::atomic<int> ack_winners{0}, loss_winners{0};
    for (int i=0; i<16; ++i) {
        workers.emplace_back([&, i] {
            if (i % 2 == 0) {
                try {
                    disconnect_race.accept_verified(
                        in_flight, IpcReceiptStage::remote_accepted);
                    ++ack_winners;
                } catch(const std::invalid_argument&) { }
            } else {
                loss_winners += static_cast<int>(
                    disconnect_race.mark_transport_lost(in_flight));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(ack_winners.load() <= 1 && loss_winners.load() == 1);
    assert(disconnect_race.counters() ==
        (std::pair<std::size_t,std::size_t>{0,0}));
    assert(disconnect_race.invariant());
    rejects([&]{ disconnect_race.accept_verified(
        in_flight, IpcReceiptStage::handler_committed); });
    // Loss yields exact receipt IDs + stage, so a local supervisor can
    // complete guest Futures with an indeterminate outcome and NO replay.
    NativeIpcReceiptTable notices(3);
    auto m1 = envelope(1), m2 = envelope(2), m3 = envelope(3);
    m2.session = "sessionB";
    notices.admit(m1);
    notices.admit(m2);
    notices.admit(m3);
    notices.commit_dispatch(m3);
    notices.accept_verified(m3, IpcReceiptStage::remote_accepted);
    auto retired = notices.retire_transport_lost(m1);
    assert(retired.size() == 2);
    assert(retired[0].envelope.message_sequence == 1);
    assert(retired[0].last_observed == IpcReceiptStage::local_admitted);
    assert(retired[1].envelope.message_sequence == 3);
    assert(retired[1].last_observed == IpcReceiptStage::remote_accepted);
    assert(notices.counters() ==
        (std::pair<std::size_t,std::size_t>{1,1}));
    assert(notices.retire_transport_lost(m1).empty());
    auto bad_epoch=m1; bad_epoch.session="bad session";
    rejects([&]{ notices.retire_transport_lost(bad_epoch); });
    notices.commit_dispatch(m2);
    notices.accept_verified(m2,IpcReceiptStage::remote_accepted);
    notices.accept_verified(m2,IpcReceiptStage::handler_committed);
    assert(notices.invariant());
    std::mt19937_64 rng(20261018);
    for(int trial=0;trial<2048;++trial) {
        NativeIpcReceiptTable book(3);
        std::uint64_t seq=1;
        for(int turn=0;turn<30;++turn) {
            auto msg=envelope(seq++);
            try {
                book.admit(msg);
                switch(rng()%4) {
                  case 0:
                    book.reject_before_remote(msg);
                    break;
                  case 1:
                    book.commit_dispatch(msg);
                    assert(book.mark_transport_lost(msg) == 1);
                    break;
                  default:
                    book.commit_dispatch(msg);
                    book.accept_verified(msg,IpcReceiptStage::remote_accepted);
                    book.accept_verified(msg,(rng()%2) ? IpcReceiptStage::handler_committed : IpcReceiptStage::handler_failed);
                    break;
                }
            } catch (const std::overflow_error&) { }
            assert(book.invariant());
        }
    }
    std::cout << "native-ipc-receipts PASS 2048x30 interleavings\n";
}
