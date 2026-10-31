// Source-free broker stub exercising the REAL NativeOresVMNio C ABI at the
// request-registration boundary. This is not a live kernel epoll/VM test.
#include "oreslang/runtime/NativeRequestNioWatch.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>

using namespace oreslang::runtime;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(false)
static bool accept_watch=false;
static unsigned watch_calls=0;
extern "C" int ores_native_vm_nio_try_watch(
        OresNativeVM* vm,int fd,uint32_t events,OresNativeNioWatch* output) {
    if(!vm || !output || fd<0 || !events) return 0;
    ++watch_calls;
    if(!accept_watch) return 0;
    *output=OresNativeNioWatch{0xabc123,1};
    return 1;
}

int main() {
    using Registry=NativeRequestContext<4>;
    using Watch=NativeRequestNioWatch<4>;
    Registry requests;
    const OresNativeActorRef owner{4,1,2};
    const AsyncIoTicket ticket{4,2,99};
    auto context=requests.open(owner,Registry::Metadata{100,200,300,400});
    CHECK(context);
    PinnedContinuationGate gate(ticket,{4,1,2});
    alignas(std::max_align_t) unsigned char unused_vm_storage[32]{};
    auto* vm=reinterpret_cast<OresNativeVM*>(unused_vm_storage);
    CHECK(!Watch::register_watch(requests,*context,owner,gate,ticket,
                                  nullptr,10,1));
    CHECK(watch_calls==0 && gate.request_binding_state()==0);
    CHECK(!Watch::register_watch(requests,*context,owner,gate,ticket,
                                  vm,-1,1));
    CHECK(watch_calls==0);
    CHECK(!Watch::register_watch(requests,*context,owner,gate,ticket,
                                  vm,10,0));
    CHECK(watch_calls==0);
    CHECK(!Watch::register_watch(requests,*context,owner,gate,ticket,
                                  vm,10,1));
    CHECK(watch_calls==1 && gate.request_binding_state()==0 &&
          requests.available()==3);
    accept_watch=true;
    auto physical=Watch::register_watch(requests,*context,owner,gate,ticket,
                                         vm,10,1);
    CHECK(physical && watch_calls==2 &&
          gate.request_binding_state()==2 && requests.available()==3);
    const OresNativeNioCompletion valid{
        physical->watch,10,1,ORES_NATIVE_NIO_READY};
    CHECK(physical->matches_completion(valid));
    auto mismatch=valid;
    mismatch.watch.ticket++;
    CHECK(!physical->matches_completion(mismatch));
    CHECK(!physical->observe_terminal_completion(mismatch));
    mismatch=valid;
    mismatch.watch.reactor++;
    CHECK(!physical->matches_completion(mismatch));
    CHECK(!physical->observe_terminal_completion(mismatch));
    mismatch=valid;
    mismatch.fd=11;
    CHECK(!physical->matches_completion(mismatch));
    CHECK(!physical->observe_terminal_completion(mismatch));
    CHECK(gate.terminal()==AsyncIoTerminal::pending);
    CHECK(!Watch::register_watch(requests,*context,owner,gate,ticket,
                                  vm,10,1));
    CHECK(watch_calls==2 && !gate.try_retire(ticket));
    CHECK(requests.close(*context,owner));
    CHECK(requests.available()==3);
    // A popped matching completion does not by itself prove broker quiescence.
    CHECK(!physical->physical.finish_after_broker_and_resume_quiescence());
    CHECK(physical->observe_terminal_completion(valid));
    CHECK(gate.terminal()==AsyncIoTerminal::success);
    CHECK(!physical->observe_terminal_completion(valid));
    CHECK(!physical->physical.finish_after_broker_and_resume_quiescence());
    // Test host explicitly supplies broker ACK and scheduler cleanup.
    // Production must derive this from actual kernel-watch lifecycle.
    CHECK(gate.broker_acknowledge(ticket));
    CHECK(gate.try_claim_resume(ticket) && gate.finish_resume(ticket));
    CHECK(physical->physical.finish_after_broker_and_resume_quiescence());
    physical.reset();
    CHECK(requests.available()==4);
    CHECK(!gate.try_bind_request(ticket));
}
