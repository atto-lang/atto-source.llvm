#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
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
    NativeIpcReceiptTable table(2);
    auto first=envelope(1), second=envelope(2), third=envelope(3);
    table.admit(first);
    assert(table.counters() == (std::pair<std::size_t,std::size_t>{1,1}));
    rejects([&]{ table.admit(first); });
    auto forgery=first; forgery.session="other";
    rejects([&]{ table.accept_verified(forgery,IpcReceiptStage::remote_accepted); });
    table.accept_verified(first,IpcReceiptStage::remote_accepted);
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::rejected_before_remote); });
    table.admit(second);
    rejects([&]{ table.admit(third); });
    table.accept_verified(first,IpcReceiptStage::handler_failed);
    rejects([&]{ table.accept_verified(first,IpcReceiptStage::handler_committed); });
    table.accept_verified(second,IpcReceiptStage::outcome_unknown);
    assert(table.counters() == (std::pair<std::size_t,std::size_t>{0,0}));
    auto rolled=envelope(3); rolled.route_epoch=3;
    rejects([&]{ table.admit(rolled); });
    rolled=envelope(3); rolled.capability_generation=4;
    rejects([&]{ table.admit(rolled); });
    table.admit(third);
    auto oldinc=envelope(4); ++oldinc.destination_incarnation;
    rejects([&]{ table.admit(oldinc); });
    table.accept_verified(third,IpcReceiptStage::outcome_unknown);
    assert(table.invariant());
    // Competing authenticated receipts may race on distinct transport carriers;
    // exactly one terminal transition must win without double-credit release.
    NativeIpcReceiptTable racing(1);
    auto race_message = envelope();
    racing.admit(race_message);
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
                    disconnect_race.mark_transport_lost("session"));
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
    std::mt19937_64 rng(20261018);
    for(int trial=0;trial<2048;++trial) {
        NativeIpcReceiptTable book(3);
        std::uint64_t seq=1;
        for(int turn=0;turn<30;++turn) {
            auto msg=envelope(seq++);
            try {
                book.admit(msg);
                switch(rng()%4) {
                  case 0: book.accept_verified(msg,IpcReceiptStage::rejected_before_remote); break;
                  case 1: book.accept_verified(msg,IpcReceiptStage::outcome_unknown); break;
                  default:
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
