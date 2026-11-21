// Real Linux VM + epoll + request-owned I/O integration, host-only.
// The VM's NIO backend is physically closed/joined BEFORE any broker ACK.
// Scheduler-owned guest continuation lowering is NOT exercised here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oreslang/runtime/NativeRequestNioWatch.hpp"
#include "oreslang/runtime/NativeOresVM.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

using namespace oreslang::runtime;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)

extern "C" void unexpected_actor_callback(void*,size_t,std::uint64_t) {
    std::abort(); // epoll completions must NEVER execute guest actor callbacks
}
static OresNativeVMConfig make_config() {
    OresNativeVMConfig cfg{};
    for (int i=0;i<ORES_NATIVE_VM_DOMAIN_COUNT;++i) {
        cfg.domains[i].actor_slots=2;
        cfg.domains[i].mailbox_capacity=4;
        cfg.domains[i].carrier_workers=1;
        cfg.domains[i].on_message=unexpected_actor_callback;
    }
    return cfg;
}
static void run_real_broker_case(bool cancellation) {
    using Registry=NativeRequestContext<4>;
    using Watch=NativeRequestNioWatch<4>;
    const auto cfg=make_config();
    OresNativeVM* vm=ores_native_vm_open(&cfg);
    CHECK(vm);
    CHECK(ores_native_vm_enable_nio(vm,1,0,1));
    CHECK(ores_native_vm_nio_started(vm)==0);
    const OresNativeActorRef actor=ores_native_vm_spawn(
        vm,ORES_NATIVE_VM_MAIN,0);
    CHECK(actor.scheduler_domain && actor.generation);
    Registry context;
    const Registry::Metadata info{
        cancellation?1102u:1101u,2201u,3301u,4401u};
    auto request=context.open(actor,info);CHECK(request);
    NativeHttpExchange exchange;
    auto exchange_pin=context.attach_exchange(exchange,*request,actor);
    CHECK(exchange_pin);
    const auto exchange_token=exchange_pin->token();
    CHECK(exchange.rollback(exchange_token));
    const AsyncIoTicket operation{
        actor.scheduler_domain,actor.generation,
        cancellation?202u:201u};
    PinnedContinuationGate gate(operation,{
        actor.scheduler_domain,actor.actor_slot,actor.generation});
    int ends[2]{-1,-1};
    CHECK(::pipe2(ends,O_NONBLOCK|O_CLOEXEC)==0);
    std::optional<Watch::AcceptedWatch> accepted;
    // Native try_watch is deliberately nonparking; bounded retry belongs
    // solely to this host test harness, never to the guest actor scheduler.
    const auto deadline=std::chrono::steady_clock::now()+
                        std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        auto attempt=Watch::register_watch(context,*request,actor,gate,
                                            operation,vm,ends[0],EPOLLIN);
        if(attempt) {
            accepted.emplace(std::move(*attempt));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(accepted);
    CHECK(gate.request_binding_state()==2);
    CHECK(ores_native_vm_nio_started(vm)==1);
    CHECK(!context.close(Registry::Handle{0},actor));
    CHECK(context.close(*request,actor));
    CHECK(!context.lookup(*request,actor));
    CHECK(context.available()==3);
    CHECK(!exchange_pin->ready_to_retire());
    CHECK(!accepted->physical.finish_after_broker_and_resume_quiescence());
    if(cancellation) {
        bool cancelled=false;
        for(int i=0;i<3000 && !cancelled;++i) {
            cancelled=ores_native_vm_nio_try_cancel(vm,accepted->watch)!=0;
            if(!cancelled)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(cancelled);
        CHECK(!ores_native_vm_nio_try_cancel(vm,accepted->watch));
    } else {
        CHECK(::write(ends[1],"x",1)==1);
    }
    OresNativeNioCompletion event{};
    bool delivered=false;
    const auto finish_by=std::chrono::steady_clock::now()+
                          std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<finish_by) {
        if(ores_native_vm_nio_try_pop(vm,&event)) {
            delivered=true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(delivered);
    CHECK(accepted->matches_completion(event));
    CHECK(event.outcome==(cancellation?ORES_NATIVE_NIO_CANCELLED:
                                        ORES_NATIVE_NIO_READY));
    auto stale=event;
    ++stale.watch.ticket;
    CHECK(!accepted->observe_terminal_completion(stale));
    CHECK(gate.terminal()==AsyncIoTerminal::pending);
    CHECK(accepted->observe_terminal_completion(event));
    CHECK(!accepted->observe_terminal_completion(event));
    CHECK(gate.terminal()==(cancellation?AsyncIoTerminal::cancelled:
                                        AsyncIoTerminal::success));
    CHECK(!accepted->physical.finish_after_broker_and_resume_quiescence());
    CHECK(!exchange_pin->ready_to_retire());
    // This is the actual broker lifecycle barrier: no more epoll worker
    // can reach the watched FD after native VM close joins/destroys it.
    CHECK(ores_native_vm_close(vm));
    vm=nullptr;
    CHECK(context.available()==3);
    CHECK(!accepted->physical.finish_after_broker_and_resume_quiescence());
    CHECK(gate.broker_acknowledge(operation));
    CHECK(!accepted->physical.finish_after_broker_and_resume_quiescence());
    // Trusted host test models a FINISHED owning scheduler cleanup turn.
    // Actual source `await` lowering and carrier authority are NOT proven.
    CHECK(gate.try_claim_resume(operation));
    CHECK(gate.finish_resume(operation));
    CHECK(accepted->physical.finish_after_broker_and_resume_quiescence());
    CHECK(gate.retired() && gate.request_binding_state()==3);
    accepted.reset();
    CHECK(exchange_pin->ready_to_retire());
    exchange_pin.reset();
    CHECK(context.available()==4);
    CHECK(exchange.enqueue(actor));
    CHECK(exchange.token().generation>exchange_token.generation);
    CHECK(exchange.rollback(exchange.token()));
    CHECK(::close(ends[0])==0);
    CHECK(::close(ends[1])==0);
}
int main() {
    run_real_broker_case(false);
    run_real_broker_case(true);
}
