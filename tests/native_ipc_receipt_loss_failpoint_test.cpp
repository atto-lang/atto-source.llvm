#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <cstdio>

void set_ipc_test_allocation_failure(int count) noexcept;
#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
using namespace oreslang::runtime;
static IpcReceiptEnvelope msg(std::uint64_t i) {
    IpcReceiptEnvelope x;
    x.domain="tenant";x.sender="sender";x.destination="worker";x.session="session";
    x.payload_sha256=std::string(64,'a');
    x.sender_incarnation=1;x.destination_incarnation=1;x.route_epoch=1;
    x.capability_generation=1;x.message_sequence=i;x.frame_sequence=i;
    return x;
}
int main() {
  for (int fail_at=0;fail_at<6;fail_at++) {
    NativeIpcReceiptTable table(4);
    table.admit(msg(1));table.admit(msg(2));
    auto second=msg(2);
    table.commit_dispatch(second);
    table.accept_verified(second,IpcReceiptStage::remote_accepted);
    bool threw=false;
    set_ipc_test_allocation_failure(fail_at);
    try {(void)table.retire_transport_lost(msg(1));}
    catch (const std::bad_alloc&) {threw=true;}
    set_ipc_test_allocation_failure(-1);
    assert(table.invariant());
    if (threw) {
        assert(table.counters()==(std::pair<std::size_t,std::size_t>{2,1}));
        auto losses=table.retire_transport_lost(msg(1));
        assert(losses.size()==2 && losses[0].envelope.message_sequence==1);
        assert(losses[1].last_observed==IpcReceiptStage::remote_accepted);
    } else {
        assert(table.counters()==(std::pair<std::size_t,std::size_t>{0,0}));
    }
  }
  std::puts("native-ipc-loss-failpoints PASS");
}
