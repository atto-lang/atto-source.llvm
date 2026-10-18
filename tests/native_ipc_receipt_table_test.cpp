#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
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
